// DupliX 2 (design v4 P1): a graph driven by keyed macros (tests/ae/run_dx2_graph.ps1 sends the steps; AE stays
// open between them so the idle hook writes). Two 400x300 comps, 3 s at 30 fps:
//   A  a 40x40 red solid with DupliX 2 as it comes (the 5 x 5 grid preset): Macro 1 (Count) keyed 2 -> 4 and
//      Macro 2 (Spacing) keyed 60 -> 100 over 0..2 s. Frames a_0, a_1, a_2.
//   B  a 30x30 green solid; the step "setup" sets Random Seed, which opens the editor on it and runs the editor
//      script in DUPLIX2_TEST_SCRIPT (a circle of 6 with its Radius exposed to Macro 1, turned 45 degrees and at
//      50% opacity) as an edit. The step "check" renders b_before (Macro 1 as the write set it: 300), then keys
//      Macro 1 50 -> 150 over 0..2 s and renders b_0, b_1, b_2, then undoes three times (the editor reloads).
//      The step "close" removes B's effect (the editor closes).
// The effect dumps the elements of every PreRender (DUPLIX2_DUMP); check_dx2_graph.py compares them with
// graph_test.exe eval and reads the pixels.
function dx2(id) { return "DupliX2-" + ("000" + id).slice(-4); }  // a parameter by its index (= Disk ID)
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    log("jsx start " + new Date().toTimeString());  // the script reached AE (before anything else)
    var stepFile = new File(outDir + "/step.txt");
    stepFile.open("r");
    var step = stepFile.readln().replace(/^﻿/, "").replace(/\s+$/, "");
    stepFile.close();
    function done() {
        var f = new File(outDir + "/step_done_" + step + ".txt");
        f.open("w"); f.writeln("ok"); f.close();
    }
    function png(comp, t, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(t, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " t=" + t + " exists=" + f.exists);
    }
    function find(name) {
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i).name == name) return app.project.item(i);
        return null;
    }
    var MACRO1 = 5, MACRO2 = 6, SEED = 3;
    try {
        app.beginSuppressDialogs();
        log("step " + step);
        if (step == "setup") {
            app.newProject();
            app.project.bitsPerChannel = parseInt($.getenv("DUPLIX_DX2_BPC") || "8", 10);
            var a = app.project.items.addComp("A", 400, 300, 1, 3, 30);
            var ra = a.layers.addSolid([1, 0, 0], "RedA", 40, 40, 1, 3);
            var fa = ra.property("ADBE Effect Parade").addProperty("DupliX2");
            log("effect " + fa.matchName + " params=" + fa.numProperties);
            for (var i = 1; i <= fa.numProperties; i++) {
                var p = fa.property(i);
                log(" #" + i + " " + p.matchName + " [" + p.name + "]");
            }
            var m1 = fa.property(dx2(MACRO1)), m2 = fa.property(dx2(MACRO2));
            log("macro1 " + m1.name + " = " + m1.value + ", macro2 " + m2.name + " = " + m2.value);
            m1.setValueAtTime(0, 2); m1.setValueAtTime(2, 4);
            m2.setValueAtTime(0, 60); m2.setValueAtTime(2, 100);

            var b = app.project.items.addComp("B", 400, 300, 1, 3, 30);
            var gb = b.layers.addSolid([0, 1, 0], "GreenB", 30, 30, 1, 3);
            var fb = gb.property("ADBE Effect Parade").addProperty("DupliX2");
            fb.property(dx2(SEED)).setValue(1);  // runs the editor script for seed 1 (DUPLIX2_TEST_SCRIPT)

            // C: Expose on the default grid lands on Macro 4, a row hidden until then; opening the editor on C
            // also closes B's (whose write is still queued)
            var c = app.project.items.addComp("C", 400, 300, 1, 3, 30);
            var bc = c.layers.addSolid([0, 0, 1], "BlueC", 20, 20, 1, 3);
            var fc = bc.property("ADBE Effect Parade").addProperty("DupliX2");
            fc.property(dx2(SEED)).setValue(2);  // the script for seed 2

            // E: the Overview's Circle preset button (the same function, seed 4); D: the editor driven by the mouse
            // (seed 3, opened last so its editor is the one open when run_dx2_graph.ps1 sends the mouse)
            var e = app.project.items.addComp("E", 400, 300, 1, 3, 30);
            var we = e.layers.addSolid([1, 1, 0], "YellowE", 35, 35, 1, 3);
            we.property("ADBE Effect Parade").addProperty("DupliX2").property(dx2(SEED)).setValue(4);
            var dd = app.project.items.addComp("D", 400, 300, 1, 3, 30);
            var wd = dd.layers.addSolid([1, 1, 1], "WhiteD", 25, 25, 1, 3);
            wd.property("ADBE Effect Parade").addProperty("DupliX2").property(dx2(SEED)).setValue(3);
            log("setup ok (" + app.project.bitsPerChannel + " bpc)");
        } else if (step == "check") {
            var a = find("A"), b = find("B"), c = find("C");
            png(a, 0, "a_0"); png(a, 1, "a_1"); png(a, 2, "a_2");
            var fc = c.layer(1).property("ADBE Effect Parade").property(1);
            log("c macro4 " + fc.property(dx2(8)).name + " = " + fc.property(dx2(8)).value + " (Expose sets 80)");
            png(c, 0, "c_0");
            png(find("D"), 0, "d_0");
            png(find("E"), 0, "e_0");
            var fb = b.layer(1).property("ADBE Effect Parade").property(1);
            var m1 = fb.property(dx2(MACRO1));
            log("b macro1 " + m1.name + " = " + m1.value + " (the write sets 300)");
            png(b, 0, "b_before");
            m1.setValueAtTime(0, 50); m1.setValueAtTime(2, 150);
            png(b, 0, "b_0"); png(b, 1, "b_1"); png(b, 2, "b_2");
            // undo step by step (the two keys, the later writes of the other comps, then B's write) until B's Macro 1
            // is back where the write found it (5, no keys): B's default grid again
            var undos = 0;
            while ((m1.numKeys > 0 || Math.abs(m1.value - 5) > 1e-6) && undos < 20) { app.executeCommand(16); undos++; }
            log("b after " + undos + " undos: macro1 = " + m1.value + " keys " + m1.numKeys);
            png(b, 0, "b_undo");
            // the answer for a copy at 50%: AE's own green solid at 50% opacity, as saveFrameToPng writes it
            var r = app.project.items.addComp("Ref", 400, 300, 1, 1, 30);
            var rs = r.layers.addSolid([0, 1, 0], "Half", 400, 300, 1, 1);
            rs.property("ADBE Transform Group").property("ADBE Opacity").setValue(50);
            png(r, 0, "ref_half");
            // and opaque ones (how saveFrameToPng writes this depth)
            rs.property("ADBE Transform Group").property("ADBE Opacity").setValue(100);
            png(r, 0, "ref_green");
            var rr = r.layers.addSolid([1, 0, 0], "Red", 400, 300, 1, 1);
            png(r, 0, "ref_red");
        } else if (step == "close") {
            // B's effect deleted (it stays in the undo history), then the project closed: the editor closes and
            // lets its reference go at SEQUENCE_SETDOWN (P0: AE crashed when a reference was used after app.open)
            var b = find("B");
            b.layer(1).property("ADBE Effect Parade").property(1).remove();
            log("b's effect removed");
            $.sleep(500);
            app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES);
            log("project closed");
        }
        log("step " + step + " end");
        done();
        if (step == "close") log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
        done();
    }
})();
