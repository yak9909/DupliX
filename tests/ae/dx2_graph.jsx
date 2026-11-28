// DupliX 2 (design v4 P1): a graph driven by keyed macros (tests/ae/run_dx2_graph.ps1 sends the steps; AE stays
// open between them so the idle hook writes). Two 400x300 comps, 3 s at 30 fps:
//   A  a 40x40 red solid with DupliX 2 as it comes (the 5 x 5 grid preset): Macro 1 (Count) keyed 2 -> 4 and
//      Macro 2 (Spacing) keyed 60 -> 100 over 0..2 s. Frames a_0, a_1, a_2.
//   B  a 30x30 green solid; the step "setup" sets Random Seed, which runs the editor script in DUPLIX2_TEST_SCRIPT
//      (a circle of 6 with its Radius exposed to Macro 1, turned 45 degrees and at 50% opacity) through the
//      editor's write. The step "check" renders b_before (Macro 1 as the write set it: 300), then keys Macro 1
//      50 -> 150 over 0..2 s and renders b_0, b_1, b_2.
// The effect dumps the elements of every PreRender (DUPLIX2_DUMP); check_dx2_graph.py compares them with
// graph_test.exe eval and reads the pixels.
function dx2(id) { return "DupliX2-" + ("000" + id).slice(-4); }  // a parameter by its index (= Disk ID)
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
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
            fb.property(dx2(SEED)).setValue(1);  // runs the editor script (DUPLIX2_TEST_SCRIPT)
            log("setup ok");
        } else if (step == "check") {
            var a = find("A"), b = find("B");
            png(a, 0, "a_0"); png(a, 1, "a_1"); png(a, 2, "a_2");
            var fb = b.layer(1).property("ADBE Effect Parade").property(1);
            var m1 = fb.property(dx2(MACRO1));
            log("b macro1 " + m1.name + " = " + m1.value + " (the write sets 300)");
            png(b, 0, "b_before");
            m1.setValueAtTime(0, 50); m1.setValueAtTime(2, 150);
            png(b, 0, "b_0"); png(b, 1, "b_1"); png(b, 2, "b_2");
            // undo takes back the two keys, then the editor's write (one step): the default grid again
            app.executeCommand(16); app.executeCommand(16); app.executeCommand(16);
            log("b after 3 undos: macro1 = " + m1.value + " keys " + m1.numKeys);
            png(b, 0, "b_undo");
            // the answer for a copy at 50%: AE's own green solid at 50% opacity, as saveFrameToPng writes it
            var r = app.project.items.addComp("Ref", 400, 300, 1, 1, 30);
            var rs = r.layers.addSolid([0, 1, 0], "Half", 400, 300, 1, 1);
            rs.property("ADBE Transform Group").property("ADBE Opacity").setValue(50);
            png(r, 0, "ref_half");
        }
        log("step " + step + " end");
        done();
        if (step == "check") log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
        done();
    }
})();
