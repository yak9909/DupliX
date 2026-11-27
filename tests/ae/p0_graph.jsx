// P0 (design v4, section 7): the graph editor's window writes the graph data; does one Ctrl+Z take a write back?
// spike/P0Graph (DupliXP0) fills the layer with its graph data's color. With DUPLIX_P0_AUTO set, its editor window
// (opened by switching "Open Editor" on) writes red after 1.5 s and green after 3 s from its own message loop, each
// as an undo group. Sent twice to a running AE by tests/ae/run_p0_graph.ps1 (AE's main loop runs in between):
//   step "setup": a comp with the effect, before.png (gray), the editor opened
//   step "check": written.png (green), undo1.png (red), undo2.png (gray), redo.png (red)
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
            e.property("DupliXP0-0001").setValue(1);  // Open Editor
            log("open editor set, value now " + e.property("DupliXP0-0001").value);
            log("setup done");
        } else {
            png("written");
            app.executeCommand(16);  // Edit > Undo
            png("undo1");
            app.executeCommand(16);
            png("undo2");
            app.executeCommand(2035);  // Edit > Redo
            png("redo");
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
