// Fast mode checks (plan docs/superpowers/plans/2026-11-24-fast-mode.md, Task 4).
// For every case builds fm_<case> (fast mode on) and nm_<case> (the same scene, normal mode), puts both in
// the render queue (TIFF r_<comp>_[#####].tif), saves fast.aep. In this AE session it also saves:
//   fast_anim: single_<frame>.png  (fm_fast_anim frames rendered one by one, out of order)
//   fast_toggle: tog_on.png (fast on), tog_off.png (switched off by script), tog_nm.png (normal mode comp)
// Env: RANDAMU_TEST_DIR (output), RANDAMU_FAST_IMAGE (png source), RANDAMU_FAST_ONLY (comma list, optional).
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var imgItem = null;
    var W = 400, H = 300;
    var FAST = 15;  // effect property index of "Fast Mode" (= RP_FAST)

    var cases = [
        // one copy, no ranges: the plane must put the drawing exactly where the real layer is
        {name: "fast_identity", image: true, params: {count: 1}, targetRot: [25, -30, 10], camMoved: true},
        {name: "fast_scatter", image: true, params: {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]}},
        {name: "fast_anim", image: true, params: {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]},
         anim: true, frames: 30},
        {name: "fast_half", image: true, params: {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]},
         half: true},
        {name: "fast_nocam", image: true, params: {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]},
         noCam: true},
        {name: "fast_count0", image: true, params: {count: 0}},
        {name: "fast_toggle", image: true, params: {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]},
         toggle: true},
        // an effect before randamu grows the layer: the input's origin is negative
        {name: "fast_grow", image: true, params: {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]},
         shadow: true},
        // removing the effect while fast mode is on (same session): the plain layer again (PNGs only)
        {name: "fast_remove", image: true, params: {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]},
         remove: true}
    ];

    function setParams(fx, p, fast) {
        var pos = p.pos || [0, 0, 0], rot = p.rot || [0, 0, 0], sc = p.scale || [100, 100], op = p.opacity || [100, 100];
        fx.property(2).setValue(p.count);
        fx.property(3).setValue(p.seed || 0);
        for (var k = 0; k < 3; k++) { fx.property(4 + k).setValue(pos[k]); fx.property(7 + k).setValue(rot[k]); }
        fx.property(10).setValue(sc[0]); fx.property(11).setValue(sc[1]);
        fx.property(12).setValue(op[0]); fx.property(13).setValue(op[1]);
        fx.property(FAST).setValue(fast ? 1 : 0);
    }
    function build(c, fast, noFx) {
        var comp = app.project.items.addComp((noFx ? "plain_" : fast ? "fm_" : "nm_") + c.name, W, H, 1, c.frames ? c.frames / 30 : 2, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        // another 3D layer behind the copies (the plane must sort in front of it)
        var back = comp.layers.addSolid([0.2, 0.3, 0.6], "Back", 300, 200, 1);
        back.threeDLayer = true;
        tg(back).property("ADBE Position").setValue([200, 150, 900]);
        var cam = null;
        if (!c.noCam) {
            cam = comp.layers.addCamera("Cam", [200, 150]);
            if (c.camMoved) {
                tg(cam).property("ADBE Position").setValue([120, 60, -700]);
                tg(cam).property("ADBE Anchor Point").setValue([230, 170, 100]);  // point of interest
            }
        }
        var T;
        if (c.image) {
            if (!imgItem) imgItem = app.project.importFile(new ImportOptions(new File($.getenv("RANDAMU_FAST_IMAGE"))));
            T = comp.layers.add(imgItem);
        } else {
            T = comp.layers.addSolid([1, 1, 1], "Target", 100, 100, 1);
        }
        T.name = "Target";
        T.threeDLayer = true;
        // Accepts Lights off (fast mode ignores lights; there are none here anyway)
        tg(T).property("ADBE Position").setValue([200, 140, 0]);
        if (c.targetRot) {
            tg(T).property("ADBE Rotate X").setValue(c.targetRot[0]);
            tg(T).property("ADBE Rotate Y").setValue(c.targetRot[1]);
            tg(T).property("ADBE Rotate Z").setValue(c.targetRot[2]);
        }
        if (c.anim) {
            // layer time != comp time: the plane table is keyed by layer time
            T.startTime = -0.4;
            var tp = tg(T).property("ADBE Position");
            tp.setValueAtTime(0, [140, 120, 0]);
            tp.setValueAtTime(29 / 30, [260, 170, 150]);
            tg(T).property("ADBE Rotate Y").setValueAtTime(0, 0);
            tg(T).property("ADBE Rotate Y").setValueAtTime(29 / 30, 40);
            var cp = tg(cam).property("ADBE Position");
            var p0 = cp.value;
            cp.setValueAtTime(0, [p0[0] - 150, p0[1] - 60, p0[2]]);
            cp.setValueAtTime(29 / 30, [p0[0] + 150, p0[1] + 40, p0[2] + 250]);
        }
        if (c.shadow) {
            var ds = T.property("ADBE Effect Parade").addProperty("ADBE Drop Shadow");
            ds.property("ADBE Drop Shadow-0004").setValue(25);  // distance
            ds.property("ADBE Drop Shadow-0005").setValue(10);  // softness
        }
        if (noFx) return comp;
        var fx = T.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
        setParams(fx, c.params, fast);
        if (c.half) comp.resolutionFactor = [2, 2];
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
        om.file = new File(outDir + "/r_" + comp.name + "_[#####].tif");
        rq.timeSpanStart = 0;
        rq.timeSpanDuration = (c.frames || 1) / 30;
        if (c.half) {
            var set = false;
            try { rq.setSetting("Resolution", "Half"); set = true; } catch (eh) { log("setSetting Resolution failed: " + eh.message); }
            log("half: resolution setting " + (set ? "set" : "not set"));
        }
    }
    function png(comp, name, time) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(time || 0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists);
    }
    function fxOf(comp) {
        for (var i = 1; i <= comp.numLayers; i++)
            if (comp.layer(i).name === "Target") {
                var par = comp.layer(i).property("ADBE Effect Parade");
                for (var k = 1; k <= par.numProperties; k++)
                    if (par.property(k).matchName === "RANDAMU Scatter") return par.property(k);
            }
        return null;
    }

    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var only = $.getenv("RANDAMU_FAST_ONLY") || "";
        for (var i = 0; i < cases.length; i++) {
            var c = cases[i];
            if (only && ("," + only + ",").indexOf("," + c.name + ",") < 0) continue;
            var fm = build(c, true), nm = build(c, false);
            if (c.remove) {
                png(fm, "rm_on", 0);
                fxOf(fm).remove();
                png(fm, "rm_off", 0);
                png(build(c, false, true), "rm_plain", 0);
                log("built " + c.name);
                continue;
            }
            addRQ(fm, c);
            addRQ(nm, c);
            if (c.anim) {
                var order = [29, 3, 17, 0, 11, 24, 8, 21];
                for (var k = 0; k < order.length; k++) png(fm, "single_" + order[k], order[k] / 30);
            }
            if (c.toggle) {
                png(fm, "tog_on", 0);
                fxOf(fm).property(FAST).setValue(0);
                png(fm, "tog_off", 0);
                png(nm, "tog_nm", 0);
                fxOf(fm).property(FAST).setValue(1);  // the render queue renders it in fast mode
            }
            log("built " + c.name);
        }
        app.project.save(new File(outDir + "/fast.aep"));
        log("saved");
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
