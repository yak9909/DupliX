// THROWAWAY SPIKE — builds spike/logs/s1/s1.aep with S1/S2 test comps.
// Run via spike/tools/run_s1.ps1 (reads env DUPLIX_SPIKE_DIR).
(function () {
    var outDir = $.getenv("DUPLIX_SPIKE_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }

    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        log("AE version " + app.version + " lang " + app.isoLanguage);
        try { log("workingSpace=" + app.project.workingSpace + " linearBlending=" + app.project.linearBlending +
                  " colorManagementSystem=" + app.project.colorManagementSystem); } catch (e) { log("color info err " + e); }

        var FX = "RNDM DupliXSpike";

        // om.templates sometimes comes back garbled after many render-queue items
        // ("TIFF ????? is not a valid template name"): look it up once and reuse it.
        var cachedTpl;
        var firstOmSettings = null;
        var ASCII_TPL = "RNDM_TIFF_A";
        function pickPngTemplate(om) {
            if (cachedTpl !== undefined) return cachedTpl;
            cachedTpl = pickPngTemplateUncached(om);
            return cachedTpl;
        }
        function pickPngTemplateUncached(om) {
            var t = om.templates;
            for (var i = 0; i < t.length; i++) if (/PNG/i.test(t[i])) return t[i];
            for (var i = 0; i < t.length; i++) if (/TIFF/i.test(t[i])) return t[i];
            return null;
        }

        function solidTarget(comp) { return comp.layers.addSolid([0.1, 0.4, 0.9], "Target", 400, 300, 1); }

        function makeComp(name, setup, makeTarget) {
            var comp = app.project.items.addComp(name, 1920, 1080, 1, 1 / 30, 30);
            var bg = comp.layers.addSolid([0.2, 0.2, 0.2], "BG", 1920, 1080, 1);
            var target = (makeTarget || solidTarget)(comp);
            target.threeDLayer = true;
            target.property("ADBE Transform Group").property("ADBE Position").setValue([1300, 500, 0]);
            var cam = comp.layers.addCamera("Cam", [960, 540]);
            var src = comp.layers.addSolid([1, 1, 0], "Src", 200, 200, 1);
            setup(comp, src, target, cam);

            var fx = src.property("ADBE Effect Parade").addProperty(FX);
            // S5b product-like scene: no slots, no expressions (the plugin takes everything from AEGP)
            if ($.getenv("DUPLIX_S1_NOSLOT") === "1") {
                log(name + " NOSLOT: no slots / expressions");
            } else {
            fx.property("Slot 1").setValue(target.index);
            fx.property("CompSize").expression = "[thisComp.width, thisComp.height, 0]";
            fx.property("Self O").expression = "var p=toComp([0,0]); [p[0],p[1],0]";
            fx.property("Self X").expression = "var p=toComp([100,0]); [p[0],p[1],0]";
            fx.property("Self Y").expression = "var p=toComp([0,100]); [p[0],p[1],0]";
            function slotExpr(u, v, slot) {
                return "var L=effect('DupliXSpike')('" + (slot || "Slot 1") + "');" +
                       "var p=L.toComp([" + u + "," + v + ",0]);" +
                       "var c=thisComp.activeCamera;" +
                       "var z=c ? c.fromWorld(L.toWorld([" + u + "," + v + ",0]))[2] : -1;" +
                       "[p[0],p[1],z]";
            }
            fx.property("S1 C0").expression = slotExpr(0, 0);
            fx.property("S1 C1").expression = slotExpr(100, 0);
            fx.property("S1 C2").expression = slotExpr(0, 100);

            // S6: a track matte layer named "Matte" goes into Slot 2 (spike only: in the product the
            // effect must find and map it by itself)
            var names = ["CompSize", "Self O", "Self X", "Self Y", "S1 C0", "S1 C1", "S1 C2"];
            var matteL = null;
            for (var li = 1; li <= comp.numLayers; li++) if (comp.layer(li).name === "Matte") matteL = comp.layer(li);
            if (matteL) {
                fx.property("Slot 2").setValue(matteL.index);
                fx.property("S2 C0").expression = slotExpr(0, 0, "Slot 2");
                fx.property("S2 C1").expression = slotExpr(100, 0, "Slot 2");
                fx.property("S2 C2").expression = slotExpr(0, 100, "Slot 2");
                names.push("S2 C0", "S2 C1", "S2 C2");
            }

            // report expression errors / values as seen by scripting
            for (var i = 0; i < names.length; i++) {
                var p = fx.property(names[i]);
                log(name + " " + names[i] + " = " + p.value.toString() +
                    (p.expressionError ? "  EXPR ERROR: " + p.expressionError : ""));
            }
            log(name + " renderer=" + comp.renderer + " available=" + comp.renderers.join(","));
            log(name + " Slot 1 value(index)=" + fx.property("Slot 1").value);
            }

            // reference: identical comp with the effect disabled = what AE itself draws
            var ref = comp.duplicate();
            ref.name = name + "_ref";
            // every layer carrying the effect ("Src", "Src2", ...) is off in the reference
            for (var ri = 1; ri <= ref.numLayers; ri++) {
                var rl = ref.layer(ri);
                if (rl.name.indexOf("Src") !== 0) continue;
                var rfx = rl.property("ADBE Effect Parade");
                for (var rj = 1; rj <= rfx.numProperties; rj++) rfx.property(rj).enabled = false;
                rl.enabled = false;
            }

            function addRQ(c, n) {
                var rq = app.project.renderQueue.items.add(c);
                var om = rq.outputModule(1);
                var tpl = pickPngTemplate(om);
                // The localized template name ("TIFF シーケンス (アルファ付き)") gets garbled after many
                // RQ items, and setSettings(getSettings(...)) does not carry the format over (S6 run:
                // items 2+ stayed "def" = H.264). So save the first one under an ASCII name and apply that.
                if (firstOmSettings) {
                    om.applyTemplate(ASCII_TPL);
                } else if (tpl) {
                    om.applyTemplate(tpl);
                    om.saveAsTemplate(ASCII_TPL);
                    firstOmSettings = true;
                }
                om.file = new File(outDir + "/" + n + "_[#####]" + (tpl && /TIFF/i.test(tpl) ? ".tif" : ".png"));
                var st = om.getSettings(GetSettingsFormat.STRING);
                log(n + " -> template " + tpl + " format=" + st["Format"] + " file=" + om.file.fsName);
            }
            addRQ(comp, name);
            addRQ(ref, name + "_ref");
            return comp;
        }

        // v0: effect layer untouched (centered 200x200 2D solid)
        makeComp("v0_identity", function (comp, src, target, cam) {});
        // v1: moved / scaled / rotated effect layer
        makeComp("v1_moved", function (comp, src, target, cam) {
            var t = src.property("ADBE Transform Group");
            t.property("ADBE Position").setValue([300, 800]);
            t.property("ADBE Scale").setValue([50, 50]);
            t.property("ADBE Rotate Z").setValue(30);
        });
        // v2: anchor point offset + upscale
        makeComp("v2_anchor", function (comp, src, target, cam) {
            var t = src.property("ADBE Transform Group");
            t.property("ADBE Anchor Point").setValue([20, 150]);
            t.property("ADBE Scale").setValue([150, 120]);
        });
        // v3: target pushed back in Z (camera projection must shrink it)
        makeComp("v3_targetZ", function (comp, src, target, cam) {
            target.property("ADBE Transform Group").property("ADBE Position").setValue([1300, 500, 800]);
        });

        // ---- S2b: what does checkout_layer return for other layer types?
        var assets = $.getenv("DUPLIX_SPIKE_ASSETS");
        var discItem = app.project.importFile(new ImportOptions(new File(assets + "/disc.png")));
        function noop() {}

        makeComp("t_png", noop, function (comp) { var l = comp.layers.add(discItem); l.name = "Target"; return l; });

        makeComp("t_text", noop, function (comp) {
            var l = comp.layers.addText("Rnd");
            var td = l.property("ADBE Text Properties").property("ADBE Text Document");
            var doc = td.value; doc.fontSize = 160; doc.applyFill = true; doc.fillColor = [0.9, 0.9, 0.2];
            td.setValue(doc);
            l.name = "Target"; return l;
        });

        makeComp("t_shape", noop, function (comp) {
            var l = comp.layers.addShape();
            var g = l.property("ADBE Root Vectors Group").addProperty("ADBE Vector Group");
            var c = g.property("ADBE Vectors Group");
            var el = c.addProperty("ADBE Vector Shape - Ellipse");
            el.property("ADBE Vector Ellipse Size").setValue([360, 220]);
            var rc = c.addProperty("ADBE Vector Shape - Rect");
            rc.property("ADBE Vector Rect Size").setValue([120, 300]);
            rc.property("ADBE Vector Rect Position").setValue([150, 0]);
            c.addProperty("ADBE Vector Graphic - Fill").property("ADBE Vector Fill Color").setValue([0.3, 0.9, 0.4]);
            l.name = "Target"; return l;
        });

        function makePrecomp(n) {
            var pc = app.project.items.addComp(n, 400, 300, 1, 1 / 30, 30);
            var a = pc.layers.addSolid([0.8, 0.3, 0.3], "A", 200, 200, 1);
            a.property("ADBE Transform Group").property("ADBE Rotate Z").setValue(30);
            a.property("ADBE Transform Group").property("ADBE Position").setValue([120, 150]);
            var d = pc.layers.add(discItem);
            d.property("ADBE Transform Group").property("ADBE Scale").setValue([60, 60]);
            d.property("ADBE Transform Group").property("ADBE Position").setValue([290, 150]);
            return pc;
        }
        makeComp("t_precomp", noop, function (comp) { var l = comp.layers.add(makePrecomp("PC_plain")); l.name = "Target"; return l; });

        makeComp("t_mask", noop, function (comp) {
            var l = solidTarget(comp);
            var m = l.property("ADBE Mask Parade").addProperty("ADBE Mask Atom");
            var s = new Shape(); s.vertices = [[0, 0], [400, 0], [0, 300]]; s.closed = true;
            m.property("ADBE Mask Shape").setValue(s);
            return l;
        });

        makeComp("t_effect", noop, function (comp) {
            var l = solidTarget(comp);
            var tr = l.property("ADBE Effect Parade").addProperty("ADBE Geometry2");
            tr.property("ADBE Geometry2-0002").setValue([300, 150]);   // Position: shift content +100px
            return l;
        });

        // S6: track mattes. Solid matte (alpha / inverted alpha), and a two-tone shape matte
        // (opaque white rect + opaque black rect) for luma, where alpha and luma differ.
        function solidMatte(comp) {
            var matte = comp.layers.addSolid([1, 1, 1], "Matte", 250, 250, 1);
            matte.threeDLayer = true;
            matte.property("ADBE Transform Group").property("ADBE Position").setValue([1250, 470, 0]);
            return matte;
        }
        function twoToneMatte(comp) {
            var matte = comp.layers.addShape();
            matte.name = "Matte";
            var root = matte.property("ADBE Root Vectors Group");
            function rect(x, col) {
                var g = root.addProperty("ADBE Vector Group");
                var c = g.property("ADBE Vectors Group");
                var r = c.addProperty("ADBE Vector Shape - Rect");
                r.property("ADBE Vector Rect Size").setValue([200, 260]);
                r.property("ADBE Vector Rect Position").setValue([x, 0]);
                c.addProperty("ADBE Vector Graphic - Fill").property("ADBE Vector Fill Color").setValue(col);
            }
            rect(-100, [1, 1, 1]);
            rect(100, [0, 0, 0]);
            matte.threeDLayer = true;
            matte.property("ADBE Transform Group").property("ADBE Position").setValue([1250, 470, 0]);
            return matte;
        }
        function matteComp(name, makeMatte, type) {
            makeComp(name, noop, function (comp) {
                var l = solidTarget(comp);
                var matte = makeMatte(comp);
                l.setTrackMatte(matte, type);
                matte.enabled = false;
                return l;
            });
        }
        matteComp("t_matte", solidMatte, TrackMatteType.ALPHA);
        matteComp("t_matte_inv", solidMatte, TrackMatteType.ALPHA_INVERTED);
        matteComp("t_matte_luma", twoToneMatte, TrackMatteType.LUMA);
        matteComp("t_matte_nluma", twoToneMatte, TrackMatteType.LUMA_INVERTED);

        // S4: layers AE does not draw must not occlude: video switch off / outside in-out range.
        makeComp("t_hidden", noop, function (comp) { var l = solidTarget(comp); l.enabled = false; return l; });
        // S7: layers AE never draws must not be targets
        makeComp("t_null", noop, function (comp) { var l = comp.layers.addNull(); l.name = "Target"; return l; });
        makeComp("t_guide", noop, function (comp) { var l = solidTarget(comp); l.guideLayer = true; return l; });
        makeComp("t_adjust", noop, function (comp) { var l = solidTarget(comp); l.adjustmentLayer = true; return l; });
        // S7: solo. Only soloed layers are drawn: "Other" (and our Src); Target must not occlude.
        makeComp("t_solo", function (comp, src, target, cam) {
            var o = comp.layers.addSolid([0.9, 0.5, 0.1], "Other", 300, 200, 1);
            o.threeDLayer = true;
            o.property("ADBE Transform Group").property("ADBE Position").setValue([700, 700, 0]);
            o.solo = true;
            src.solo = true;
            src.moveToBeginning();   // the effect layer must be on top (design 5.4)
        });
        // S7: a second layer with this effect (must be skipped, and must not recurse / hang)
        makeComp("t_two_fx", function (comp, src, target, cam) {
            var s2 = comp.layers.addSolid([0, 1, 1], "Src2", 100, 100, 1);
            s2.property("ADBE Effect Parade").addProperty(FX);
        });

        // no camera layer: AE uses its default camera. Target pushed back so a missing projection shows.
        makeComp("t_nocam", function (comp, src, target, cam) {
            cam.remove();
            target.property("ADBE Transform Group").property("ADBE Position").setValue([1300, 500, 500]);
        });
        makeComp("t_outrange", noop, function (comp) { var l = solidTarget(comp); l.inPoint = 0.5; return l; });

        // Last: in S5 run 1, AE crashed right after the AEGP render of a collapsed precomp.
        makeComp("t_collapse", noop, function (comp) {
            var l = comp.layers.add(makePrecomp("PC_collapse"));
            l.collapseTransformation = true;
            l.name = "Target"; return l;
        });

        var aep = new File(outDir + "/s1.aep");
        app.project.save(aep);
        log("saved " + aep.fsName);
    } catch (e) {
        log("EXCEPTION: " + e.toString() + " line " + e.line);
    }
    app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES);
    app.quit();
})();
