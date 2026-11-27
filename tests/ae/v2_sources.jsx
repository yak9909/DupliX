// DupliX 2 (prototype): several layers as Sources. A 400x300 comp with three 100x100 solids (red, green, blue;
// the blue one with its video switch off) and a 400x300 grey "Target" with DupliX 2: 3 x 1 copies in a 300 x 100
// grid, so the cell centres are (100,150), (200,150), (300,150). Saves:
//   self.png        no Source: the copies show the Target itself (grey)
//   seq.png         Sources red, green, blue, Assign Sequential -> red, green, blue
//   bya_rev.png     Sources blue, green, red (set in that order), Assign By A -> blue, green, red
//   gap.png         Source 2 = None (red, -, blue): the empty slot is skipped -> red, blue, red
//   seq_<n>.png     the project at $DUPLIX_V2_BPC bits (8 or 32)
// tests/ae/check_v2_sources.py reads the colours. jsx.log lists the parameters (match name, name).
function dx2(id) { return "DupliX2-" + ("000" + id).slice(-4); }  // a DupliX 2 parameter by its index (= disk ID)
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function png(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists);
    }
    // indices (src/v2/dx2_params.h)
    var P_COUNT_A = 3, P_COUNT_B = 4, P_GRID_SIZE_X = 9, P_GRID_SIZE_Y = 10;
    var P_SRC_LAYER = -1, P_SRC_ASSIGN = -1;
    try {
        // every row shown: a script cannot set a hidden parameter (Source 2.. are hidden until used)
        $.setenv("DUPLIX2_SHOW_ALL", "1");
        app.beginSuppressDialogs();
        app.newProject();
        var bpc = parseInt($.getenv("DUPLIX_V2_BPC") || "8", 10);
        app.project.bitsPerChannel = bpc;
        var comp = app.project.items.addComp("v2", 400, 300, 1, 1, 30);
        var red = comp.layers.addSolid([1, 0, 0], "Red", 100, 100, 1);
        var green = comp.layers.addSolid([0, 1, 0], "Green", 100, 100, 1);
        var blue = comp.layers.addSolid([0, 0, 1], "Blue", 100, 100, 1);
        red.enabled = false;
        green.enabled = false;
        blue.enabled = false;  // a hidden source still works (the usual way to keep the originals out of sight)
        var T = comp.layers.addSolid([0.5, 0.5, 0.5], "Target", 400, 300, 1);
        var fx = T.property("ADBE Effect Parade").addProperty("DupliX2");
        log("effect " + (fx ? fx.matchName : "null") + " params=" + fx.numProperties);
        for (var i = 1; i <= fx.numProperties; i++) {
            var p = fx.property(i);
            log(" #" + i + " " + p.matchName + " [" + p.name + "]");
            if (p.name === "Source 1") P_SRC_LAYER = parseInt(p.matchName.slice(-4), 10);
            if (p.name === "Assign") P_SRC_ASSIGN = parseInt(p.matchName.slice(-4), 10);
        }
        log("P_SRC_LAYER=" + P_SRC_LAYER + " P_SRC_ASSIGN=" + P_SRC_ASSIGN);
        fx.property(dx2(P_COUNT_A)).setValue(3);
        fx.property(dx2(P_COUNT_B)).setValue(1);
        fx.property(dx2(P_GRID_SIZE_X)).setValue(300);
        fx.property(dx2(P_GRID_SIZE_Y)).setValue(100);
        // layers: 1 Target (added last goes on top), 2 Blue, 3 Green, 4 Red
        log("layers: " + comp.layer(1).name + "," + comp.layer(2).name + "," + comp.layer(3).name + "," + comp.layer(4).name);
        png(comp, "self");

        function src(k, layer) { fx.property(dx2(P_SRC_LAYER + k)).setValue(layer ? layer.index : 0); }
        src(0, red); src(1, green); src(2, blue);
        fx.property(dx2(P_SRC_ASSIGN)).setValue(1);  // Sequential
        png(comp, "seq");
        png(comp, "seq_" + bpc);

        src(0, blue); src(1, green); src(2, red);
        fx.property(dx2(P_SRC_ASSIGN)).setValue(3);  // By A
        png(comp, "bya_rev");

        src(0, red); src(1, null); src(2, blue);
        fx.property(dx2(P_SRC_ASSIGN)).setValue(1);
        png(comp, "gap");

        // reference: a red solid without the effect (how saveFrameToPng writes this depth)
        var refComp = app.project.items.addComp("ref", 400, 300, 1, 1, 30);
        refComp.layers.addSolid([1, 0, 0], "RefRed", 400, 300, 1);
        png(refComp, "ref_red");
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
