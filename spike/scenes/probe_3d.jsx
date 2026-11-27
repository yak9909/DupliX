// THROWAWAY SPIKE — sub-layer copies vs real duplicated layers, in a classic-3D scene with a light,
// shadows, a floor and a wall that intersects the copies.
// "probe": Target (+ hidden effect layer T whose SEQUENCE_SETUP installs the fill hooks, copies from
//          the fixed table, DUPLIX_FILL_TABLE=1).
// "ref":   5 real duplicates of Target at the same offsets.
// Env: DUPLIX_SPIKE_DIR. Writes jsx.log, f_probe.png, f_ref.png.
(function () {
    var outDir = $.getenv("DUPLIX_SPIKE_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function frame(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        try {
            comp.saveFrameToPng(parseFloat($.getenv("DUPLIX_3D_TIME") || "0"), f);
            for (var i = 0; i < 100 && !f.exists; i++) $.sleep(100);
            log("frame " + name + " exists=" + f.exists);
        } catch (e) { log("frame " + name + " EXC " + e); }
    }
    var table = [[0, 0, 0], [-110, -40, 80], [110, 30, -120], [-60, 60, -200], [70, -60, 150]];
    var P0 = [200, 140, 0];
    function scene(comp) {
        log("scene " + comp.name);
        comp.layers.addSolid([0.1, 0.1, 0.1], "BG", 400, 300, 1);
        log(" bg");
        var floor = comp.layers.addSolid([0.6, 0.6, 0.6], "Floor", 1200, 1200, 1);
        floor.threeDLayer = true;
        floor.property("ADBE Transform Group").property("ADBE Position").setValue([200, 230, 0]);
        floor.property("ADBE Transform Group").property("ADBE Rotate X").setValue(90);
        log(" floor");
        var wall = comp.layers.addSolid([0.2, 0.3, 1], "Wall", 400, 300, 1);
        wall.threeDLayer = true;
        wall.property("ADBE Transform Group").property("ADBE Position").setValue([240, 140, 0]);
        wall.property("ADBE Transform Group").property("ADBE Rotate Y").setValue(90);
        log(" wall");
        var light = comp.layers.addLight("Light", [200, 150]);
        log(" light added type=" + light.lightType);
        light.lightType = LightType.SPOT;
        log(" light type=" + light.lightType);
        light.property("ADBE Transform Group").property("ADBE Position").setValue([120, -150, -250]);
        log(" light pos");
        if ($.getenv("DUPLIX_3D_LIGHTDUMP") === "1") {
            var lo = light.property("ADBE Light Options Group");
            log("  opts n=" + lo.numProperties);
            for (var q = 1; q <= lo.numProperties; q++) {
                log("  opt#" + q + " get");
                var pq = lo.property(q);
                log("  opt#" + q + " match=" + pq.matchName);
                log("  opt#" + q + " name=" + pq.name);
                var vq = "";
                try { vq = pq.value; } catch (eq) { vq = "(err " + eq + ")"; }
                log("  opt#" + q + " value=" + vq);
            }
        }
        if ($.getenv("DUPLIX_3D_SHADOWS") === "1") {
            // Reading "ADBE Light Color" (option #6) blocks the hidden-desktop AE on a modal dialog, and a
            // lookup by match name seems to touch it too: walk the options by index from #7, match names only.
            var lo2 = light.property("ADBE Light Options Group");
            for (var q2 = 7; q2 <= lo2.numProperties; q2++) {
                var p2 = lo2.property(q2);
                log("  opt#" + q2 + " " + p2.matchName + " [" + p2.name + "]");
                if (p2.matchName === "ADBE Light Casts Shadows" || p2.matchName === "ADBE Casts Shadows") { p2.setValue(1); log(" light shadows (#" + q2 + ")"); }
            }
        }
        comp.layers.addCamera("Cam", [200, 150]);
        log(" cam");
    }
    var imgItem = null;
    function makeTarget(comp, name, off) {
        var L;
        var imgPath = $.getenv("DUPLIX_3D_IMAGE");
        if (imgPath) {
            if (!imgItem) imgItem = app.project.importFile(new ImportOptions(new File(imgPath)));
            L = comp.layers.add(imgItem);
            L.name = name;
        } else {
            L = comp.layers.addSolid([1, 1, 1], name, 100, 100, 1);
        }
        L.threeDLayer = true;
        L.property("ADBE Transform Group").property("ADBE Position").setValue([P0[0] + off[0], P0[1] + off[1], P0[2] + off[2]]);
        if ($.getenv("DUPLIX_3D_SHADOWS") === "1") {
            var mo = L.property("ADBE Material Options Group");
            for (var q3 = 1; q3 <= mo.numProperties; q3++)
                if (mo.property(q3).matchName === "ADBE Casts Shadows") mo.property(q3).setValue(1);
        }
        return L;
    }
    log("start");
    // DUPLIX_3D_EXTRA: comma list of dof, adj, mb
    var extraOpt = "," + ($.getenv("DUPLIX_3D_EXTRA") || "") + ",";
    function extra(comp) {
        var cam = null;
        for (var i = 1; i <= comp.numLayers; i++) if (comp.layer(i).name === "Cam") cam = comp.layer(i);
        if (extraOpt.indexOf(",dof,") >= 0) {
            var co = cam.property("ADBE Camera Options Group");
            co.property("ADBE Camera Depth of Field").setValue(1);
            co.property("ADBE Camera Aperture").setValue(120);
            log(" dof");
        }
        if (extraOpt.indexOf(",mb,") >= 0) {
            comp.motionBlur = true;
            for (var j = 1; j <= comp.numLayers; j++) {
                var Lj = comp.layer(j);
                if (Lj.name === "Target" || Lj.name.indexOf("Copy") === 0) Lj.motionBlur = true;
            }
            if (extraOpt.indexOf(",cammove,") >= 0) {
                var cp = cam.property("ADBE Transform Group").property("ADBE Position");
                var p0 = cp.value;
                cp.setValueAtTime(0, p0);
                cp.setValueAtTime(1, [p0[0] + 600, p0[1], p0[2]]);
            }
            log(" mb");
        }
        if (extraOpt.indexOf(",adj,") >= 0) {
            var adj = comp.layers.addSolid([1, 1, 1], "Adj", 400, 300, 1);
            adj.adjustmentLayer = true;
            adj.property("ADBE Effect Parade").addProperty("ADBE Gaussian Blur 2").property("ADBE Gaussian Blur 2-0001").setValue(4);
            log(" adj");
        }
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var ref = app.project.items.addComp("ref", 400, 300, 1, 2, 30);
        scene(ref);
        var rot = [[0, 0, 0], [0, 0, 30], [0, 40, 0], [35, 0, 0], [20, 30, 40]];
        var useRot = $.getenv("DUPLIX_FILL_ROT") === "1";
        var nRand = parseInt($.getenv("DUPLIX_FILL_RANDOM") || "0", 10);
        if (nRand > 0) {
            // same Park-Miller sequence as RandomXform() in the plugin (products stay below 2^53)
            var sx = 12345;
            function nx() { sx = (sx * 16807) % 2147483647; return sx / 2147483647; }
            table = []; rot = [];
            for (var r0 = 0; r0 < nRand; r0++) {
                var a1 = (nx() - 0.5) * 300, a2 = (nx() - 0.5) * 200, a3 = (nx() - 0.5) * 600;
                var b1 = (nx() - 0.5) * 120, b2 = (nx() - 0.5) * 120, b3 = (nx() - 0.5) * 120;
                table.push([a1, a2, a3]); rot.push([b1, b2, b3]);
            }
            log("random table n=" + nRand + " first=" + table[0].join(",") + " rot0=" + rot[0].join(","));
        }
        var noRef = $.getenv("DUPLIX_3D_NOREF") === "1";  // timing runs: no reference copies
        for (var k = 0; k < (noRef ? 0 : table.length); k++) {
            var C = makeTarget(ref, "Copy" + k, table[k]);
            if ($.getenv("DUPLIX_FILL_OPACITY") === "1")
                C.property("ADBE Transform Group").property("ADBE Opacity").setValue(100 * (0.3 + 0.7 * ((k * 37) % 10) / 9));
            if ($.getenv("DUPLIX_FILL_ANIM") === "1") {
                // copy k moves +300 px in x per second (same as the plugin's time-dependent offset)
                var pp = C.property("ADBE Transform Group").property("ADBE Position");
                var pv = pp.value;
                pp.setValueAtTime(0, pv);
                pp.setValueAtTime(1, [pv[0] + 300, pv[1], pv[2]]);
            }
            if (useRot) {
                var tg = C.property("ADBE Transform Group");
                tg.property("ADBE Rotate X").setValue(rot[k][0]);
                tg.property("ADBE Rotate Y").setValue(rot[k][1]);
                tg.property("ADBE Rotate Z").setValue(rot[k][2]);
            }
        }
        // DUPLIX_3D_MULTI: a second target ("Target2", slider 5 -> offsets x0.5) at P1
        var multi = $.getenv("DUPLIX_3D_MULTI") === "1";
        var P1 = [80, 40, 120];  // offset of Target2 from P0
        if (multi) {
            for (var k3 = 0; k3 < table.length; k3++) {
                var C3 = makeTarget(ref, "Copy2_" + k3, [P1[0] + table[k3][0] / 2, P1[1] + table[k3][1] / 2, P1[2] + table[k3][2] / 2]);
                if (useRot) {
                    var tg3 = C3.property("ADBE Transform Group");
                    tg3.property("ADBE Rotate X").setValue(rot[k3][0]);
                    tg3.property("ADBE Rotate Y").setValue(rot[k3][1]);
                    tg3.property("ADBE Rotate Z").setValue(rot[k3][2]);
                }
            }
        }
        extra(ref);
        var probe = app.project.items.addComp("probe", 400, 300, 1, 2, 30);
        scene(probe);
        var T1 = makeTarget(probe, "Target", [0, 0, 0]);
        if (noRef && nRand > 0) {
            // timing runs: put N into an effect parameter so the disk cache key changes with N
            T1.property("ADBE Effect Parade").addProperty("ADBE Slider Control").property("ADBE Slider Control-0001").setValue(nRand);
            log("cache key slider=" + nRand);
        }
        if (multi) {
            T1.property("ADBE Effect Parade").addProperty("ADBE Slider Control");
            var T2 = makeTarget(probe, "Target2", P1);
            T2.property("ADBE Effect Parade").addProperty("ADBE Slider Control").property("ADBE Slider Control-0001").setValue(5);
            log("multi targets");
        }
        extra(probe);
        var T = probe.layers.addSolid([0, 0, 0], "T", 10, 10, 1);
        T.property("ADBE Effect Parade").addProperty("RNDM DupliXSpike");
        T.enabled = false;
        log("built");
        frame(ref, "f_ref");
        if ($.getenv("DUPLIX_3D_CACHETEST") === "1") {
            // second reference at half the offsets
            var ref2 = app.project.items.addComp("ref2", 400, 300, 1, 2, 30);
            scene(ref2);
            for (var k2 = 0; k2 < table.length; k2++) {
                var C2 = makeTarget(ref2, "Copy" + k2, [table[k2][0] / 2, table[k2][1] / 2, table[k2][2] / 2]);
                if (useRot) {
                    var tg2 = C2.property("ADBE Transform Group");
                    tg2.property("ADBE Rotate X").setValue(rot[k2][0]);
                    tg2.property("ADBE Rotate Y").setValue(rot[k2][1]);
                    tg2.property("ADBE Rotate Z").setValue(rot[k2][2]);
                }
            }
            extra(ref2);
            frame(ref2, "f_ref2");
            var tgt = null;
            for (var m = 1; m <= probe.numLayers; m++) if (probe.layer(m).name === "Target") tgt = probe.layer(m);
            var slider = tgt.property("ADBE Effect Parade").addProperty("ADBE Slider Control");
            var sf = new File($.getenv("DUPLIX_FILL_FILE"));
            function setScale(v) { sf.open("w"); sf.write(String(v)); sf.close(); log("scale file=" + v); }
            setScale(1);
            frame(probe, "f_p1");
            setScale(0.5);
            frame(probe, "f_p2_nochange");
            slider.property("ADBE Slider Control-0001").setValue(5);
            log("slider changed");
            frame(probe, "f_p3_slider");
            log("done");
            return;
        }
        frame(probe, "f_probe");
        if ($.getenv("DUPLIX_3D_SAVE") === "1") {
            for (var m2 = 1; m2 <= probe.numLayers; m2++)
                if (probe.layer(m2).name.indexOf("Target") === 0) log("TARGET_ID=" + probe.layer(m2).id);
            function addRQ(c, n) {
                var rq = app.project.renderQueue.items.add(c);
                var om = rq.outputModule(1);
                var ok = false;
                try { om.applyTemplate("RNDM_TIFF_A"); ok = true; } catch (et) {}
                if (!ok) {
                    var t = om.templates, tpl = null;
                    for (var i = 0; i < t.length; i++) if (/TIFF/i.test(t[i])) { tpl = t[i]; break; }
                    om.applyTemplate(tpl);
                    om.saveAsTemplate("RNDM_TIFF_A");
                }
                om.file = new File(outDir + "/r_" + n + "_[#####].tif");
                rq.timeSpanStart = parseFloat($.getenv("DUPLIX_3D_TIME") || "0");
                rq.timeSpanDuration = 1 / 30;
                log("rq " + n + " format=" + om.getSettings(GetSettingsFormat.STRING)["Format"]);
            }
            addRQ(probe, "probe");
            addRQ(ref, "ref");
            app.project.save(new File(outDir + "/probe3d.aep"));
            log("saved");
        }
        log("done");
    } catch (e) {
        log("EXCEPTION " + e + " line " + e.line);
    }
})();
