// Fast mode checks (plan docs/superpowers/plans/2026-11-25-fast-mode-plugin.md, Task 4).
// For every case builds fm_<case> (fast mode: 2D layer in its initial state) and nm_<case> (the same scene in
// normal mode: 3D layer in its initial state). The placement is in the World Transform parameters, so both
// show the same copies. Both go in the render queue (TIFF r_<comp>_[#####].tif); fast.aep is saved.
// In this AE session it also saves:
//   fast_anim: single_<frame>.png  (fm_fast_anim frames rendered one by one, out of order)
//   fast_remove: rm_on.png, rm_off.png (effect removed), rm_plain.png (never had it)
// nohooks_rq.txt: the render queue index of fm_fast_nohooks (run_fast_mode.ps1 renders it again without hooks).
// Env: DUPLIX_TEST_DIR (output), DUPLIX_FAST_IMAGE (png source), DUPLIX_FAST_ONLY (comma list, optional).
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var imgItem = null;
    var W = 400, H = 300;
    var FAST = "DupliX-0037", WPOS = 17, WROT = 20, WSCALE = 23;  // Render Mode; World Transform disk IDs
    function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }  // a parameter by its disk ID

    var scatter = {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]};
    var world = {pos: [0, -10, 0], rot: [0, 0, 0]};
    var cases = [
        // one copy, no ranges: the drawing must be exactly where the real layer is
        {name: "fast_identity", params: {count: 1}, world: {pos: [0, -10, 0], rot: [25, -30, 10]}, camMoved: true},
        {name: "fast_scatter", params: scatter, world: world},
        {name: "fast_anim", params: scatter, world: world, anim: true, frames: 30},
        // only the camera moves (still image, static World Transform): AE must not reuse a cached frame
        {name: "fast_camonly", params: scatter, world: world, camAnim: true, frames: 30},
        {name: "fast_half", params: scatter, world: world, half: true},
        {name: "fast_nocam", params: scatter, world: world, noCam: true},
        {name: "fast_count0", params: {count: 0}, world: world},
        // an effect before DupliX grows the layer: the input's origin is negative
        {name: "fast_grow", params: scatter, world: world, shadow: true},
        // the layer's 2D position moves the whole drawing (compared with fm_fast_move2d_ref; no Back layer, which
        // would not move)
        {name: "fast_move2d", params: scatter, world: world, move2d: [50, 20], noBack: true},
        {name: "fast_move2d_ref", params: scatter, world: world, move2d: [0, 0], noBack: true},
        // rendered again by run_fast_mode.ps1 with DUPLIX_HOOKMASK=0 (compared with fm_fast_scatter)
        {name: "fast_nohooks", params: scatter, world: world, nohooks: true},
        // removing the effect while fast mode is on (same session): the plain layer again (PNGs only)
        {name: "fast_remove", params: scatter, world: world, remove: true}
    ];

    function setParams(fx, p, w, fast) {
        var pos = p.pos || [0, 0, 0], rot = p.rot || [0, 0, 0], sc = p.scale || [100, 100], op = p.opacity || [100, 100];
        // fast mode first: switching it on transfers the layer's transform, here the initial state (no change)
        fx.property(FAST).setValue(fast ? 2 : 1);
        fx.property("DupliX-0002").setValue(p.count);
        fx.property("DupliX-0003").setValue(p.seed || 0);
        for (var k = 0; k < 3; k++) { fx.property(dx(4 + k)).setValue(pos[k]); fx.property(dx(7 + k)).setValue(rot[k]); }
        fx.property("DupliX-0010").setValue(sc[0]); fx.property("DupliX-0011").setValue(sc[1]);
        fx.property("DupliX-0012").setValue(op[0]); fx.property("DupliX-0013").setValue(op[1]);
        for (var j = 0; j < 3; j++) {
            fx.property(dx(WPOS + j)).setValue(w.pos[j]);
            fx.property(dx(WROT + j)).setValue(w.rot[j]);
        }
    }
    function build(c, fast, noFx) {
        var comp = app.project.items.addComp((noFx ? "plain_" : fast ? "fm_" : "nm_") + c.name, W, H, 1, c.frames ? c.frames / 30 : 2, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        // another 3D layer behind the copies
        if (!c.noBack) {
            var back = comp.layers.addSolid([0.2, 0.3, 0.6], "Back", 300, 200, 1);
            back.threeDLayer = true;
            tg(back).property("ADBE Position").setValue([200, 150, 900]);
        }
        var cam = null;
        if (!c.noCam) {
            cam = comp.layers.addCamera("Cam", [200, 150]);
            if (c.camMoved) {
                tg(cam).property("ADBE Position").setValue([120, 60, -700]);
                tg(cam).property("ADBE Anchor Point").setValue([230, 170, 100]);  // point of interest
            }
        }
        if (!imgItem) imgItem = app.project.importFile(new ImportOptions(new File($.getenv("DUPLIX_FAST_IMAGE"))));
        var T = comp.layers.add(imgItem);
        T.name = "Target";
        // the initial state: anchor at the centre (default), position = comp centre, 100%, no rotation
        T.threeDLayer = !fast;
        tg(T).property("ADBE Position").setValue(fast ? [W / 2, H / 2] : [W / 2, H / 2, 0]);
        if (c.anim) T.startTime = -0.4;  // layer time != comp time
        if (cam && (c.anim || c.camAnim)) {
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
        var fx = T.property("ADBE Effect Parade").addProperty("DupliX");
        setParams(fx, c.params, c.world, fast);
        // after switching fast mode on (switching on moves the layer's transform into the World Transform)
        if (fast && c.move2d) tg(T).property("ADBE Position").setValue([W / 2 + c.move2d[0], H / 2 + c.move2d[1]]);
        if (c.anim) {
            // the World Transform animates (script key times are comp times)
            var wp = fx.property(dx(WPOS)), wr = fx.property(dx(WROT + 1)), wz = fx.property(dx(WPOS + 2));
            wp.setValueAtTime(0, -60); wp.setValueAtTime(29 / 30, 60);
            wz.setValueAtTime(0, 0); wz.setValueAtTime(29 / 30, 150);
            wr.setValueAtTime(0, 0); wr.setValueAtTime(29 / 30, 40);
        }
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
        return app.project.renderQueue.numItems;
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
                    if (par.property(k).matchName === "DupliX") return par.property(k);
            }
        return null;
    }

    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        // DUPLIX_FAST_GPU: the project's GPU acceleration (SOFTWARE = CPU only, CUDA = the GPU path)
        var gpu = $.getenv("DUPLIX_FAST_GPU") || "SOFTWARE";
        app.project.gpuAccelType = GpuAccelType[gpu];
        log("gpu set: " + app.project.gpuAccelType);
        var only = $.getenv("DUPLIX_FAST_ONLY") || "";
        for (var i = 0; i < cases.length; i++) {
            var c = cases[i];
            if (only && ("," + only + ",").indexOf("," + c.name + ",") < 0) continue;
            var fm = build(c, true);
            if (c.remove) {
                png(fm, "rm_on", 0);
                fxOf(fm).remove();
                png(fm, "rm_off", 0);
                png(build(c, true, true), "rm_plain", 0);  // the same 2D layer, never had the effect
                log("built " + c.name);
                continue;
            }
            var idx = addRQ(fm, c);
            if (c.nohooks) {
                var nf = new File(outDir + "/nohooks_rq.txt");
                nf.open("w"); nf.write(String(idx)); nf.close();
            } else if (!c.move2d) {
                addRQ(build(c, false), c);
            }
            if (gpu !== "SOFTWARE" && c.name === "fast_scatter") {
                // fast_gpu_switch: the same frame on the GPU, then with the project switched to CPU only
                png(fm, "sw_gpu", 0);
                app.project.gpuAccelType = GpuAccelType.SOFTWARE;
                png(fm, "sw_cpu", 0);
                app.project.gpuAccelType = GpuAccelType[gpu];
            }
            if (c.anim || c.camAnim) {
                var order = [29, 3, 17, 0, 11, 24, 8, 21];
                for (var k = 0; k < order.length; k++) png(fm, (c.anim ? "single_" : "camsingle_") + order[k], order[k] / 30);
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
