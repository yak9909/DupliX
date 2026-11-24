// Builds probe_<case> (Target + randamu) and ref_<case> (real duplicate layers placed from
// randamu_dump's draws) for every case in ref_cases.json, saves ref.aep with all comps in the render
// queue, then reopens it and saves probe/ref PNGs of "baseline" (reopen check).
// Env: RANDAMU_TEST_DIR (output; copies_<case>.json must be there), RANDAMU_REF_CASES (json path),
//      RANDAMU_REF_IMAGE (png for "image" cases).
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function readJson(path) {
        var f = new File(path);
        f.encoding = "UTF-8";
        f.open("r");
        var t = f.read();
        f.close();
        return eval("(" + t + ")");
    }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var P0 = [200, 140, 0];
    var imgItem = null;

    function scene(comp, c) {
        comp.layers.addSolid([0.1, 0.1, 0.1], "BG", 400, 300, 1);
        var floor = comp.layers.addSolid([0.6, 0.6, 0.6], "Floor", 1200, 1200, 1);
        floor.threeDLayer = true;
        tg(floor).property("ADBE Position").setValue([200, 230, 0]);
        tg(floor).property("ADBE Rotate X").setValue(90);
        var wall = comp.layers.addSolid([0.2, 0.3, 1], "Wall", 400, 300, 1);
        wall.threeDLayer = true;
        tg(wall).property("ADBE Position").setValue([240, 140, 0]);
        tg(wall).property("ADBE Rotate Y").setValue(90);
        var light = comp.layers.addLight("Light", [200, 150]);
        light.lightType = LightType.SPOT;
        tg(light).property("ADBE Position").setValue([120, -150, -250]);
        if (c.shadows) {
            // Reading "ADBE Light Color" (option #6) blocks the hidden AE on a dialog: walk from #7, match names only.
            var lo = light.property("ADBE Light Options Group");
            for (var q = 7; q <= lo.numProperties; q++)
                if (lo.property(q).matchName === "ADBE Casts Shadows") lo.property(q).setValue(1);
        }
        comp.layers.addCamera("Cam", [200, 150]);
    }
    function camOf(comp) {
        for (var i = 1; i <= comp.numLayers; i++) if (comp.layer(i).name === "Cam") return comp.layer(i);
        return null;
    }
    function extras(comp, c) {
        var ex = "," + (c.extra || []).join(",") + ",";
        var cam = camOf(comp);
        if (ex.indexOf(",dof,") >= 0) {
            var co = cam.property("ADBE Camera Options Group");
            co.property("ADBE Camera Depth of Field").setValue(1);
            co.property("ADBE Camera Aperture").setValue(120);
        }
        if (ex.indexOf(",mb,") >= 0) {
            comp.motionBlur = true;
            for (var j = 1; j <= comp.numLayers; j++) {
                var Lj = comp.layer(j);
                if (Lj.name === "Target" || Lj.name.indexOf("Copy") === 0) Lj.motionBlur = true;
            }
            if (ex.indexOf(",cammove,") >= 0) {
                var cp = tg(cam).property("ADBE Position");
                var p0 = cp.value;
                cp.setValueAtTime(0, p0);
                cp.setValueAtTime(1, [p0[0] + 600, p0[1], p0[2]]);
            }
        }
        if (ex.indexOf(",adj,") >= 0) {
            var adj = comp.layers.addSolid([1, 1, 1], "Adj", 400, 300, 1);
            adj.adjustmentLayer = true;
            adj.property("ADBE Effect Parade").addProperty("ADBE Gaussian Blur 2").property("ADBE Gaussian Blur 2-0001").setValue(4);
        }
    }
    function parentNull(comp, c) {
        if (!c.parent) return null;
        var n = comp.layers.addNull();
        n.name = "Parent";
        n.threeDLayer = true;
        tg(n).property("ADBE Position").setValue([210, 150, 20]);
        tg(n).property("ADBE Rotate Z").setValue(15);
        return n;
    }
    // A layer that looks like the target (solid or PNG, optional Fill effect, shadows).
    function makeSource(comp, c, name) {
        var L;
        if (c.image) {
            if (!imgItem) imgItem = app.project.importFile(new ImportOptions(new File($.getenv("RANDAMU_REF_IMAGE"))));
            L = comp.layers.add(imgItem);
            L.name = name;
        } else {
            L = comp.layers.addSolid([1, 1, 1], name, 100, 100, 1);
        }
        L.threeDLayer = true;
        if (c.fill) {
            var fill = L.property("ADBE Effect Parade").addProperty("ADBE Fill");
            fill.property("ADBE Fill-0002").setValue([1, 0.2, 0.2, 1]);
        }
        if (c.shadows) {
            var mo = L.property("ADBE Material Options Group");
            for (var q = 1; q <= mo.numProperties; q++)
                if (mo.property(q).matchName === "ADBE Casts Shadows") mo.property(q).setValue(1);
        }
        return L;
    }
    // Places a layer like the target: anchor (+ offset), position P0, own rotation/scale/opacity, parent.
    function placeLikeTarget(L, c, parent) {
        var a = tg(L).property("ADBE Anchor Point").value;
        if (c.anchorOffset) {
            a = [a[0] + c.anchorOffset[0], a[1] + c.anchorOffset[1], 0];
            tg(L).property("ADBE Anchor Point").setValue(a);
        }
        if (parent) L.setParentWithJump(parent);
        tg(L).property("ADBE Position").setValue(parent ? [P0[0] - 210, P0[1] - 150, P0[2] - 20] : P0);
        if (c.targetRot) {
            tg(L).property("ADBE Rotate X").setValue(c.targetRot[0]);
            tg(L).property("ADBE Rotate Y").setValue(c.targetRot[1]);
            tg(L).property("ADBE Rotate Z").setValue(c.targetRot[2]);
        }
        if (c.targetScale) tg(L).property("ADBE Scale").setValue([c.targetScale, c.targetScale, c.targetScale]);
        return a;
    }
    function setParams(fx, p) {
        var pos = p.pos || [0, 0, 0], rot = p.rot || [0, 0, 0], sc = p.scale || [100, 100], op = p.opacity || [100, 100];
        fx.property(2).setValue(p.count);
        fx.property(3).setValue(p.seed || 0);
        for (var k = 0; k < 3; k++) { fx.property(4 + k).setValue(pos[k]); fx.property(7 + k).setValue(rot[k]); }
        fx.property(10).setValue(sc[0]); fx.property(11).setValue(sc[1]);
        fx.property(12).setValue(op[0]); fx.property(13).setValue(op[1]);
        fx.property(14).setValue(p.keepFirst ? 1 : 0);
    }

    function buildProbe(c) {
        var comp = app.project.items.addComp("probe_" + c.name, 400, 300, 1, 2, 30);
        scene(comp, c);
        var parent = parentNull(comp, c);
        var T = makeSource(comp, c, "Target");
        placeLikeTarget(T, c, parent);
        if (c.targetOpacity !== undefined) tg(T).property("ADBE Opacity").setValue(c.targetOpacity);
        var fx = T.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
        setParams(fx, c.params);
        extras(comp, c);
        if (c.fxOff) fx.enabled = false;
        if (c.fxDelete) fx.remove();
        log("probe " + c.name + " TARGET_ID=" + T.id + " COMP_ID=" + comp.id);
        if (c.dupRemoveFx) {
            // The original keeps its copies; the duplicate (same layer IDs?) must render the plain target.
            var dup = comp.duplicate();
            dup.name = "probe_" + c.name + "_dup";
            var D = null;
            for (var i = 1; i <= dup.numLayers; i++) if (dup.layer(i).name === "Target") D = dup.layer(i);
            D.property("ADBE Effect Parade").property(1).remove();
            log("dup " + dup.name + " TARGET_ID=" + D.id + " COMP_ID=" + dup.id);
            return dup;
        }
        return comp;
    }
    function buildRef(c) {
        var comp = app.project.items.addComp("ref_" + c.name, 400, 300, 1, 2, 30);
        scene(comp, c);
        var parent = parentNull(comp, c);
        var mode = c.ref || "copies";
        if (mode === "plain") {
            var T = makeSource(comp, c, "Target");
            placeLikeTarget(T, c, parent);
            if (c.targetOpacity !== undefined) tg(T).property("ADBE Opacity").setValue(c.targetOpacity);
        } else if (mode === "copies") {
            // A 3D null with the target's transform: its layer space is the target's layer space.
            var frame = makeSource(comp, c, "Frame");  // same source as the target -> same default anchor
            frame.enabled = false;
            var a = placeLikeTarget(frame, c, parent);
            var draws = readJson(outDir + "/copies_" + c.name + ".json").copies;
            if (c.refManualX !== undefined) {
                // no parenting: target rotated about X only, copies unrotated -> place each copy by hand
                var ang = c.refManualX * Math.PI / 180, cs = Math.cos(ang), sn = Math.sin(ang);
                for (var m = 0; m < draws.length; m++) {
                    var t = draws[m].t;
                    var M = makeSource(comp, c, "Copy" + m);
                    tg(M).property("ADBE Anchor Point").setValue(a);
                    // row vector (x,y,z) * Rx  ->  (x, y*c - z*s, y*s + z*c)
                    tg(M).property("ADBE Position").setValue([P0[0] + t[0], P0[1] + t[1] * cs - t[2] * sn, P0[2] + t[1] * sn + t[2] * cs]);
                    tg(M).property("ADBE Rotate X").setValue(c.refManualX);
                }
                draws = [];
            }
            var baseOp = c.targetOpacity !== undefined ? c.targetOpacity : 100;
            for (var k = 0; k < draws.length; k++) {
                var d = draws[k];
                var C = makeSource(comp, c, "Copy" + k);
                tg(C).property("ADBE Anchor Point").setValue(a);
                // setParentWithJump keeps the child's values; `parent =` would rewrite its Orientation to
                // cancel the parent's X/Y rotation.
                C.setParentWithJump(frame);
                tg(C).property("ADBE Position").setValue([a[0] + d.t[0], a[1] + d.t[1], d.t[2]]);
                tg(C).property("ADBE Scale").setValue([d.s, d.s, d.s]);
                tg(C).property("ADBE Rotate X").setValue(d.r[0]);
                tg(C).property("ADBE Rotate Y").setValue(d.r[1]);
                tg(C).property("ADBE Rotate Z").setValue(d.r[2]);
                tg(C).property("ADBE Opacity").setValue(baseOp * d.o / 100);
            }
        }
        extras(comp, c);
        return comp;
    }
    function addRQ(comp, c) {
        var rq = app.project.renderQueue.items.add(comp);
        var om = rq.outputModule(1);
        var ok = false;
        try { om.applyTemplate("RNDM_TIFF_A"); ok = true; } catch (et) {}
        if (!ok) {
            var t = om.templates, tpl = null;
            for (var i = 0; i < t.length; i++) if (/TIFF/i.test(t[i])) { tpl = t[i]; break; }
            om.applyTemplate(tpl);
            om.saveAsTemplate("RNDM_TIFF_A");
        }
        om.file = new File(outDir + "/r_" + comp.name.replace(/_dup$/, "") + "_[#####].tif");
        rq.timeSpanStart = c.time || 0;
        rq.timeSpanDuration = 1 / 30;
    }
    function png(comp, name, time) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(time || 0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists);
    }

    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var cases = readJson($.getenv("RANDAMU_REF_CASES")).cases;
        var only = $.getenv("RANDAMU_REF_ONLY") || "";
        for (var i = 0; i < cases.length; i++) {
            var c = cases[i];
            if (only && ("," + only + ",").indexOf("," + c.name + ",") < 0) continue;
            var pc = buildProbe(c);
            var rc = buildRef(c);
            addRQ(pc, c);
            addRQ(rc, c);
            log("built " + c.name);
        }
        var aep = new File(outDir + "/ref.aep");
        app.project.save(aep);
        log("saved");
        // reopen check: a fresh project load must register the effect again (SEQUENCE_RESETUP)
        app.open(aep);
        var items = app.project.items;
        for (var j = 1; j <= items.length; j++) {
            var it = items[j];
            if (it.name === "probe_baseline") png(it, "reopen_probe", 0);
            if (it.name === "ref_baseline") png(it, "reopen_ref", 0);
        }
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        var f2 = new File(outDir + "/jsx.log");
        f2.encoding = "UTF-8"; f2.open("a"); f2.writeln("EXCEPTION " + msg); f2.close();
    }
})();
