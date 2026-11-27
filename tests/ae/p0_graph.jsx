// P0 (design v4, section 7): the graph editor's window writes the graph data; does one Ctrl+Z take a write back?
// spike/P0Graph (DupliXP0) fills the layer with its graph data's color. With DUPLIX_P0_AUTO set, its editor window
// (opened by switching "Open Editor" on) writes red after 1.5 s and green after 3 s from its own message loop, each
// as an undo group. Sent twice to a running AE by tests/ae/run_p0_graph.ps1 (AE's main loop runs in between):
//   step "setup": a comp with the effect, before.png (gray), the editor opened
//   step "check": written.png (green), Macro 1's name, one Ctrl+Z (the editor's idle hook must notice before "check2")
//   step "check2": undo1.png (red), undo2.png (gray), redo.png (red), names, expressions by the shown / match / defined name
// The step is read from <out>/step.txt. tests/ae/check_p0_graph.py reads the colors. Env: DUPLIX_TEST_DIR.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    var sf = new File(outDir + "/step.txt");
    sf.open("r");
    var step = sf.readln();
    sf.close();
    var got = new File(outDir + "/script_got_" + step + ".txt");
    got.open("w"); got.writeln("1"); got.close();
    function comp() {
        for (var i = 1; i <= app.project.numItems; i++)
            if (app.project.item(i).name === "p0") return app.project.item(i);
        return null;
    }
    // what a script sees of Macro 1's name (the graph data names it; PF_UpdateParamUI shows it)
    function names(when) {
        var e = comp().layer("S").property("ADBE Effect Parade").property(1);
        var m = e.property("DupliXP0-0003");
        log("names " + when + ": name '" + m.name + "' by index '" + e.property(3).name + "'");
    }
    function png(name) {
        var f = new File(outDir + "/" + name + ".png");
        comp().saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists);
    }
    try {
        app.beginSuppressDialogs();
        if (step === "setup") {
            app.newProject();
            app.project.bitsPerChannel = 8;
            var c = app.project.items.addComp("p0", 64, 64, 1, 2, 30);
            var L = c.layers.addSolid([0, 0, 1], "S", 64, 64, 1);
            var e = L.property("ADBE Effect Parade").addProperty("DupliXP0");
            log("effect " + (e ? e.matchName : "missing"));
            png("before");
            // the Effect Controls show the layer's effects (UPDATE_PARAMS_UI is sent for what is shown)
            c.openInViewer();
            L.selected = true;
            e.property("DupliXP0-0001").setValue(1);  // Open Editor
            log("open editor set, value now " + e.property("DupliXP0-0001").value);
            log("setup done");
        } else if (step === "check") {
            png("written");
            names("after the writes");
            app.executeCommand(16);  // Edit > Undo: the editor must notice (its idle hook, before the next step)
        } else if (step === "check2") {
            png("undo1");
            names("after undo");
            app.executeCommand(16);
            png("undo2");
            app.executeCommand(2035);  // Edit > Redo
            png("redo");
            names("after redo");
            // expressions on another layer: Macro 1 by the name the graph data gives it ("Spread" now), and by its
            // match name (after the undo checks: these add undo steps of their own)
            var c2 = comp(), S = c2.layer("S");
            S.property("ADBE Effect Parade").property(1).property("DupliXP0-0003").setValue(37);
            var E = c2.layers.addSolid([1, 1, 1], "E", 8, 8, 1);
            var byName = E.property("ADBE Transform Group").property("ADBE Opacity");
            byName.expression = 'thisComp.layer("S").effect("DupliX P0 (Spike)")("Spread")';
            var byMatch = E.property("ADBE Transform Group").property("ADBE Rotate Z");
            byMatch.expression = 'thisComp.layer("S").effect("DupliX P0 (Spike)")("DupliXP0-0003")';
            var E2 = c2.layers.addSolid([1, 1, 1], "E2", 8, 8, 1);
            var byOld = E2.property("ADBE Transform Group").property("ADBE Opacity");
            byOld.expression = 'thisComp.layer("S").effect("DupliX P0 (Spike)")("Macro 1")';
            log("expr by shown name: value " + byName.valueAtTime(0, false) + " error '" + byName.expressionError + "'");
            log("expr by match name: value " + byMatch.valueAtTime(0, false) + " error '" + byMatch.expressionError + "'");
            log("expr by defined name: value " + byOld.valueAtTime(0, false) + " error '" + byOld.expressionError + "'");
            app.project.save(new File(outDir + "/p0_saved.aep"));
            log("saved");
        } else {
            // step "reopen": the saved project opened again; nothing selected, the Effect Controls not showing the layer
            app.open(new File(outDir + "/p0_saved.aep"));
            var c3 = comp(), Ea = c3.layer("E"), Eb = c3.layer("E2");
            function expr(L, what) {
                var p = L.property("ADBE Transform Group").property(what === "match" ? "ADBE Rotate Z" : "ADBE Opacity");
                return "value " + p.valueAtTime(0, false) + " error '" + p.expressionError + "'";
            }
            log("reopened, before the Effect Controls: shown name " + expr(Ea, "name") + "; match name " +
                expr(Ea, "match") + "; defined name " + expr(Eb, "name"));
            png("reopened");
            c3.openInViewer();
            c3.layer("S").selected = true;
            log("reopened, layer selected: shown name " + expr(Ea, "name") + "; match name " + expr(Ea, "match") +
                "; defined name " + expr(Eb, "name"));
            log("done");
        }
    } catch (ex) {
        var msg = "?";
        try { msg = String(ex.message) + " (line " + String(ex.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
    var fin = new File(outDir + "/step_done_" + step + ".txt");
    fin.open("w"); fin.writeln("1"); fin.close();
})();
