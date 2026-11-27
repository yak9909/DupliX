// UI session test (Task 7): in the hidden-desktop AE with its UI, open a comp in the viewer, scrub the
// time 30 times and change parameters / the fx switch / undo, saving a PNG after every step.
// Also builds "kf" (rotation range keyframed 0 -> 90) and saves its frame at 0.5 s, then saves ui.aep
// with "kf" in the render queue so run_ui.ps1 can compare the UI frame with aerender.
// Writes jsx.log ("STEP n OK <what>", "done") to $DUPLIX_TEST_DIR.
function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }  // a DupliX parameter by its disk ID
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var step = 0;
    function ok(what) { step++; log("STEP " + step + " OK " + what); }
    function png(comp, name, time) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(time, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        if (!f.exists) throw new Error("png not written: " + name);
    }
    function undoable(name, fn) {
        app.beginUndoGroup(name);
        fn();
        app.endUndoGroup();
    }
    function makeComp(name) {
        var comp = app.project.items.addComp(name, 400, 300, 1, 2, 30);
        comp.layers.addSolid([0.1, 0.1, 0.1], "BG", 400, 300, 1);
        var wall = comp.layers.addSolid([0.2, 0.3, 1], "Wall", 400, 300, 1);
        wall.threeDLayer = true;
        tg(wall).property("ADBE Position").setValue([240, 140, 0]);
        tg(wall).property("ADBE Rotate Y").setValue(90);
        comp.layers.addCamera("Cam", [200, 150]);
        var L = comp.layers.addSolid([1, 1, 1], "Target", 60, 60, 1);
        L.threeDLayer = true;
        // in front of the wall; moves over the 2 s so that scrubbing changes the picture
        tg(L).property("ADBE Position").setValueAtTime(0, [160, 140, -100]);
        tg(L).property("ADBE Position").setValueAtTime(2, [240, 140, -100]);
        var fx = L.property("ADBE Effect Parade").addProperty("DupliX");
        fx.property(dx(2)).setValue(10);                                  // count
        fx.property(dx(3)).setValue(1);                                   // seed
        for (var q = 4; q <= 6; q++) fx.property(dx(q)).setValue(120);    // position range
        return {comp: comp, layer: L, fx: fx};
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var s = makeComp("ui");
        var comp = s.comp, fx = s.fx;
        comp.openInViewer();
        ok("open in viewer");

        for (var i = 0; i < 30; i++) {
            var t = (i * 2) / 30;     // 0 .. 1.93 s
            comp.time = t;
            png(comp, "scrub_" + (i < 10 ? "0" : "") + i, t);
        }
        ok("scrub 30 frames");

        comp.time = 1;
        png(comp, "a_base", 1);
        ok("base");
        undoable("count", function () { fx.property(dx(2)).setValue(20); });
        png(comp, "b_count20", 1);
        ok("count 20");
        undoable("seed", function () { fx.property(dx(3)).setValue(7); });
        png(comp, "c_seed7", 1);
        ok("seed 7");
        undoable("rot", function () { for (var r = 7; r <= 9; r++) fx.property(dx(r)).setValue(60); });
        png(comp, "d_rot60", 1);
        ok("rotation 60");
        app.executeCommand(16);   // Edit > Undo
        log("after undo: rotX=" + fx.property(dx(7)).value + " seed=" + fx.property(dx(3)).value);
        png(comp, "e_undo", 1);
        ok("undo");
        undoable("fx off", function () { fx.enabled = false; });
        png(comp, "f_fxoff", 1);
        ok("fx off");
        // reference for "fx off": a plain copy of the target without the effect, target hidden
        var plain = s.layer.duplicate();
        plain.property("ADBE Effect Parade").property(1).remove();
        s.layer.enabled = false;
        png(comp, "f_ref", 1);
        plain.remove();
        s.layer.enabled = true;
        ok("fx off reference");
        undoable("fx on", function () { fx.enabled = true; });
        png(comp, "g_fxon", 1);
        ok("fx on");

        // Keyframed rotation range: UI frame vs aerender (run_ui.ps1).
        var k = makeComp("kf");
        var rx = k.fx.property(dx(7));
        rx.setValueAtTime(0, 0);
        rx.setValueAtTime(1, 90);
        k.fx.property(dx(8)).setValue(45);
        k.comp.openInViewer();
        k.comp.time = 0.5;
        png(k.comp, "kf_ui", 0.5);
        ok("keyframed rotation (UI frame)");

        var rq = app.project.renderQueue.items.add(k.comp);
        var om = rq.outputModule(1);
        var applied = false;
        try { om.applyTemplate("RNDM_TIFF_A"); applied = true; } catch (et) {}
        if (!applied) {
            var tl = om.templates, tpl = null;
            for (var j = 0; j < tl.length; j++) if (/TIFF/i.test(tl[j])) { tpl = tl[j]; break; }
            om.applyTemplate(tpl);
            om.saveAsTemplate("RNDM_TIFF_A");
        }
        om.file = new File(outDir + "/r_kf_[#####].tif");
        rq.timeSpanStart = 0.5;
        rq.timeSpanDuration = 1 / 30;
        app.project.save(new File(outDir + "/ui.aep"));
        ok("saved ui.aep");
        // Note: AE started with -r quits when the script ends, so idle-time viewer redraws (scheduleTask)
        // cannot be tested here; saveFrameToPng renders on a render thread.
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
