// Merge optimisation checks (plan docs/superpowers/plans/2026-11-25-merge-optimization.md).
// For every case builds mg_<case> (DupliX with merge on) and, when the case compares, nm_<case> (the same scene
// with merge off). Each comp goes in the render queue (TIFF r_<comp>_[#####].tif); rq.txt lists "comp=index"
// (1-based render queue indices) so run_merge.ps1 can render each one with its own plugin log. merge.aep is saved.
// Env: RANDAMU_TEST_DIR (output), RANDAMU_MERGE_IMAGE (png source), RANDAMU_MERGE_ONLY (comma list, optional).
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var imgItem = null;
    var W = 400, H = 300;
    var MERGE = "RANDAMU Scatter-0027";  // the merge checkbox (disk ID RD_MERGE)

    var scatter = {count: 10, seed: 3, pos: [140, 90, 250], rot: [0, 0, 90], scale: [60, 130]};
    var cases = [
        // Task 2: what the hooks read (log only)
        {name: "scene_basic", params: scatter, walls: [{pos: [200, 150, 300]}, {pos: [260, 150, 600], rotY: 40, fx: true}]},
        {name: "scene_light", params: scatter, walls: [{pos: [200, 150, 300]}], light: true},
        {name: "scene_nocam", params: scatter, walls: [{pos: [200, 150, 300]}], noCam: true},
        // Task 3: one group of every copy, compared with merge off (no other surfaces)
        {name: "group_pair", params: {count: 2, seed: 5, pos: [150, 60, 100]}, compare: true},
        {name: "group_scatter", params: scatter, compare: true},
        {name: "group_half", params: scatter, compare: true, half: true},
        {name: "group_nocam", params: scatter, compare: true, noCam: true}
    ];

    function setParams(fx, p, merge) {
        var pos = p.pos || [0, 0, 0], rot = p.rot || [0, 0, 0], sc = p.scale || [100, 100], op = p.opacity || [100, 100];
        fx.property(2).setValue(p.count);
        fx.property(3).setValue(p.seed || 0);
        for (var k = 0; k < 3; k++) { fx.property(4 + k).setValue(pos[k]); fx.property(7 + k).setValue(rot[k]); }
        fx.property(10).setValue(sc[0]); fx.property(11).setValue(sc[1]);
        fx.property(12).setValue(op[0]); fx.property(13).setValue(op[1]);
        fx.property(MERGE).setValue(merge ? 1 : 0);
    }
    function build(c, merge) {
        var comp = app.project.items.addComp((merge ? "mg_" : "nm_") + c.name, W, H, 1, c.frames ? c.frames / 30 : 2, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        var walls = c.walls || [];
        for (var i = 0; i < walls.length; i++) {
            var wd = walls[i];
            var wl = comp.layers.addSolid(wd.color || [0.2 + 0.3 * i, 0.6, 0.3], "Wall" + i, wd.w || 160, wd.h || 220, 1);
            wl.threeDLayer = true;
            tg(wl).property("ADBE Position").setValue(wd.pos);
            if (wd.rotY) tg(wl).property("ADBE Rotate Y").setValue(wd.rotY);
            if (wd.fx) wl.property("ADBE Effect Parade").addProperty("ADBE Fill");
        }
        if (c.light) comp.layers.addLight("Light", [200, 150]);
        if (!c.noCam) comp.layers.addCamera("Cam", [200, 150]);
        if (!imgItem) imgItem = app.project.importFile(new ImportOptions(new File($.getenv("RANDAMU_MERGE_IMAGE"))));
        var T = comp.layers.add(imgItem);
        T.name = "Target";
        T.threeDLayer = true;
        tg(T).property("ADBE Position").setValue([W / 2, H / 2, 0]);
        var fx = T.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
        setParams(fx, c.params, merge);
        return comp;
    }
    var rqLines = [];
    function addRQ(comp, half) {
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
        rq.timeSpanDuration = 1 / 30;
        if (half) {
            try { rq.setSetting("Resolution", "Half"); } catch (eh) { log("setSetting Resolution failed: " + eh.message); }
        }
        rqLines.push(comp.name + "=" + app.project.renderQueue.numItems);
    }

    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var only = $.getenv("RANDAMU_MERGE_ONLY") || "";
        for (var i = 0; i < cases.length; i++) {
            var c = cases[i];
            if (only && ("," + only + ",").indexOf("," + c.name + ",") < 0) continue;
            addRQ(build(c, true), c.half);
            if (c.compare) addRQ(build(c, false), c.half);
            log("built " + c.name);
        }
        var f = new File(outDir + "/rq.txt");
        f.encoding = "UTF-8"; f.open("w"); f.write(rqLines.join("\n")); f.close();
        app.project.save(new File(outDir + "/merge.aep"));
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.message + " (line " + e.line + ")");
    }
})();
