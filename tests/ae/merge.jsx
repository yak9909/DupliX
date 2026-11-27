// Merge optimisation checks (plan docs/superpowers/plans/2026-11-25-merge-optimization.md).
// For every case builds mg_<case> (DupliX with merge on) and, when the case compares, nm_<case> (the same scene
// with merge off). Each comp goes in the render queue (TIFF r_<comp>_[#####].tif); rq.txt lists "comp=index"
// (1-based render queue indices) so run_merge.ps1 can render each one with its own plugin log. merge.aep is saved.
// Env: DUPLIX_TEST_DIR (output), DUPLIX_MERGE_IMAGE (png source), DUPLIX_MERGE_ONLY (comma list, optional).
function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }  // a DupliX parameter by its disk ID
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var imgItem = null;
    var W = 400, H = 300;
    var MERGE = "DupliX-0027";  // the merge checkbox (disk ID RD_MERGE)

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
        {name: "group_nocam", params: scatter, compare: true, noCam: true},
        // draft quality of all-group layers (nearest sampling): odd comp sizes and a resolution factor that does
        // not divide the group rects' 24 px alignment must keep the plane on whole pixels
        {name: "group_odd", params: scatter, compare: true, w: 401, h: 301},
        {name: "group_res5", params: scatter, compare: true, res: [5, 5]},
        {name: "group_odd_res3", params: scatter, compare: true, w: 401, h: 301, res: [3, 3]},
        // Task 4: a narrow opaque wall turned 60 deg between two copies (left and right, not overlapping it on
        // screen); run_merge.ps1 -ForceD puts the group plane through the wall's middle (in its transparent part)
        {name: "seam", params: {count: 2, seed: 16, pos: [150, 20, 0], scale: [40, 40]}, compare: true,
         walls: [{pos: [200, 150, 300], w: 40, h: 220, rotY: 60, color: [0, 0, 1]}]},
        // Task 6: the order against other layers and between layers (merge on vs off)
        {name: "wall_front_back", params: {count: 30, seed: 4, pos: [160, 100, 260], rot: [20, 40, 30], scale: [50, 90]},
         compare: true, walls: [{pos: [200, 150, 60], w: 260, h: 200, color: [0.9, 0.1, 0.1]}]},
        {name: "wall_tilted", params: {count: 30, seed: 6, pos: [150, 100, 200], rot: [0, 30, 30], scale: [50, 90]},
         compare: true, walls: [{pos: [200, 150, 0], w: 300, h: 240, rotY: 60, color: [0.1, 0.8, 0.2]}]},
        {name: "wall_side", params: {count: 20, seed: 8, pos: [170, 100, 30], scale: [40, 70]}, compare: true,
         walls: [{pos: [200, 150, 0], w: 40, h: 260, color: [0.2, 0.2, 0.9]}]},
        // a solid target: AE fills a solid's copies without a texture, so it must not merge (the same frame as merge off)
        {name: "solid_target", params: scatter, compare: true, solid: true},
        {name: "two_layers", params: {count: 15, seed: 9, pos: [150, 100, 300], rot: [0, 0, 60], scale: [50, 90]}, compare: true,
         second: {count: 15, seed: 10, pos: [150, 100, 300], rot: [0, 0, 30], scale: [50, 90]}}
    ];

    function setParams(fx, p, merge) {
        var pos = p.pos || [0, 0, 0], rot = p.rot || [0, 0, 0], sc = p.scale || [100, 100], op = p.opacity || [100, 100];
        fx.property("DupliX-0002").setValue(p.count);
        fx.property("DupliX-0003").setValue(p.seed || 0);
        for (var k = 0; k < 3; k++) { fx.property(dx(4 + k)).setValue(pos[k]); fx.property(dx(7 + k)).setValue(rot[k]); }
        fx.property("DupliX-0010").setValue(sc[0]); fx.property("DupliX-0011").setValue(sc[1]);
        fx.property("DupliX-0012").setValue(op[0]); fx.property("DupliX-0013").setValue(op[1]);
        fx.property(MERGE).setValue(merge ? 1 : 0);
    }
    function build(c, merge) {
        var W = c.w || 400, H = c.h || 300;
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
        if (!imgItem) imgItem = app.project.importFile(new ImportOptions(new File($.getenv("DUPLIX_MERGE_IMAGE"))));
        var T = c.solid ? comp.layers.addSolid([1, 0.8, 0.1], "Target", 90, 70, 1) : comp.layers.add(imgItem);
        T.name = "Target";
        T.threeDLayer = true;
        tg(T).property("ADBE Position").setValue([W / 2, H / 2, 0]);
        var fx = T.property("ADBE Effect Parade").addProperty("DupliX");
        setParams(fx, c.params, merge);
        if (c.second) {  // another DupliX layer (the same image; a solid would not merge) mixed in depth with the first
            var S = comp.layers.add(imgItem);
            S.name = "Second";
            S.threeDLayer = true;
            tg(S).property("ADBE Position").setValue([W / 2, H / 2, 0]);
            setParams(S.property("ADBE Effect Parade").addProperty("DupliX"), c.second, merge);
        }
        return comp;
    }
    var rqLines = [];
    function addRQ(comp, half, res, file) {
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
        om.file = new File(file || (outDir + "/r_" + comp.name + "_[#####].tif"));
        rq.timeSpanStart = 0;
        rq.timeSpanDuration = 1 / 30;
        if (half) {
            try { rq.setSetting("Resolution", "Half"); } catch (eh) { log("setSetting Resolution failed: " + eh.message); }
        }
        if (res) {
            // a custom resolution factor: the comp's own factor with the render setting "current settings"
            comp.resolutionFactor = res;
            try { rq.setSetting("Resolution", "Current Settings"); } catch (er) { log("setSetting Resolution (current) failed: " + er.message); }
            log("res " + comp.name + " factor " + comp.resolutionFactor.join("x"));
        }
        rqLines.push(comp.name + "=" + app.project.renderQueue.numItems);
    }

    // DUPLIX_MERGE_PASSES ("OFF,ON,ND,G,G16,..."; optional): render every comp in this AE, one render queue item at
    // a time, in each pass: OFF / ON = multi-frame rendering off / on, ND = MFR off with the draft quality of
    // all-group layers off (DUPLIX_MERGE_DRAFT=0); G = group textures drawn on the GPU (DUPLIX_MERGE_GPU_DRAW=1,
    // every other pass draws them on the CPU), C = the CPU explicitly; a trailing 16 / 32 sets the project's bits
    // per channel (8 otherwise). Each render gets its own plugin log (the plugin reads DUPLIX_LOG on every line), a
    // fresh cache salt and purged caches; outputs <comp>_<pass>.tif and plugin_<comp>_<pass>.log.
    // DUPLIX_MERGE_FORCE_SEAM ("x,y,z"): DUPLIX_MERGE_FORCE_DW for the seam comps only.
    function renderPasses(targets, passes) {
        var forceSeam = $.getenv("DUPLIX_MERGE_FORCE_SEAM") || "";
        for (var pi = 0; pi < passes.length; pi++) {
            var pass = passes[pi];
            app.setMultiFrameRenderingConfig(pass === "ON", 100);
            $.setenv("DUPLIX_MERGE_DRAFT", pass === "ND" ? "0" : "");
            $.setenv("DUPLIX_MERGE_GPU_DRAW", pass.charAt(0) === "G" ? "1" : "0");
            var bpcM = /(16|32)$/.exec(pass);
            app.project.bitsPerChannel = bpcM ? parseInt(bpcM[1], 10) : 8;
            for (var ti = 0; ti < targets.length; ti++) {
                var tg0 = targets[ti], name = tg0.comp.name;
                var t0 = new Date().getTime();
                $.setenv("DUPLIX_LOG", (outDir + "/plugin_" + name + "_" + pass + ".log").replace(/\//g, "\\"));
                $.setenv("DUPLIX_CACHE_SALT", "merge-" + pass + "-" + name + "-" + t0);
                $.setenv("DUPLIX_MERGE_FORCE_DW", (forceSeam && /_seam$/.test(name)) ? forceSeam : "");
                var old = new File(outDir + "/plugin_" + name + "_" + pass + ".log");
                if (old.exists) old.remove();
                app.purge(PurgeTarget.ALL_CACHES);
                var before = app.project.renderQueue.numItems;
                addRQ(tg0.comp, tg0.half, tg0.res, outDir + "/p_" + name + "_" + pass + "_[#####].tif");
                rqLines.pop();
                app.project.renderQueue.render();
                var out = new File(outDir + "/p_" + name + "_" + pass + "_00000.tif");
                var dst = new File(outDir + "/" + name + "_" + pass + ".tif");
                if (dst.exists) dst.remove();
                if (out.exists) out.copy(dst.fsName);
                app.project.renderQueue.item(before + 1).remove();
                log("rendered " + name + " " + pass + " " + (new Date().getTime() - t0) + " ms" + (out.exists ? "" : " (no output)"));
            }
        }
        $.setenv("DUPLIX_LOG", "");
    }

    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var only = $.getenv("DUPLIX_MERGE_ONLY") || "";
        var targets = [];
        for (var i = 0; i < cases.length; i++) {
            var c = cases[i];
            if (only && ("," + only + ",").indexOf("," + c.name + ",") < 0) continue;
            var cm = build(c, true);
            addRQ(cm, c.half, c.res);
            targets.push({comp: cm, half: c.half, res: c.res});
            if (c.compare) {
                var cn = build(c, false);
                addRQ(cn, c.half, c.res);
                targets.push({comp: cn, half: c.half, res: c.res});
            }
            log("built " + c.name);
        }
        var f = new File(outDir + "/rq.txt");
        f.encoding = "UTF-8"; f.open("w"); f.write(rqLines.join("\n")); f.close();
        app.project.save(new File(outDir + "/merge.aep"));
        var passes = $.getenv("DUPLIX_MERGE_PASSES") || "";
        if (passes) {
            // the queue items made for aerender are not rendered here
            for (var q = 1; q <= app.project.renderQueue.numItems; q++) app.project.renderQueue.item(q).render = false;
            renderPasses(targets, passes.split(","));
        }
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.message + " (line " + e.line + ")");
    }
})();
