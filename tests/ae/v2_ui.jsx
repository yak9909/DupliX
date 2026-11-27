// DupliX 2 (prototype): a scene for looking at the Effect Controls (tests/ae/run_v2_ui.ps1 takes the picture).
// Three Sources, Modifier 1 = Wave, Modifier 2 = Noise; then the rows not in use are hidden again and the layer
// is selected so that the Effect Controls show the effect. Writes jsx.log ("done") to $DUPLIX_TEST_DIR.
function dx2(id) { return "DupliX2-" + ("000" + id).slice(-4); }
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    try {
        $.setenv("DUPLIX2_SHOW_ALL", "1");  // a script cannot set a hidden parameter
        app.beginSuppressDialogs();
        app.newProject();
        var comp = app.project.items.addComp("v2 ui", 1280, 720, 1, 5, 30);
        var names = ["flower_red", "flower_blue", "leaf"], cols = [[0.9, 0.3, 0.3], [0.3, 0.5, 0.95], [0.4, 0.8, 0.4]];
        var src = [];
        for (var i = 0; i < 3; i++) {
            src[i] = comp.layers.addSolid(cols[i], names[i], 200, 200, 1);
            src[i].enabled = false;
        }
        var T = comp.layers.addSolid([0, 0, 0], "DupliX 2", 1280, 720, 1);
        var fx = T.property("ADBE Effect Parade").addProperty("DupliX2");
        var idx = {};
        for (var k = 1; k <= fx.numProperties; k++) idx[fx.property(k).name] = parseInt(fx.property(k).matchName.slice(-4), 10);
        fx.property(dx2(idx["Count A"])).setValue(6);
        fx.property(dx2(idx["Count B"])).setValue(4);
        fx.property(dx2(idx["Grid Size X"])).setValue(1000);
        fx.property(dx2(idx["Grid Size Y"])).setValue(600);
        for (var s = 0; s < 3; s++) fx.property(dx2(idx["Source " + (s + 1)])).setValue(src[s].index);
        fx.property(dx2(idx["Assign"])).setValue(2);  // Random
        fx.property(dx2(idx["M1 Type"])).setValue(2);  // Wave
        fx.property(dx2(idx["M2 Type"])).setValue(3);  // Noise
        $.setenv("DUPLIX2_SHOW_ALL", "");
        // a change makes AE send UPDATE_PARAMS_UI: the rows not in use are hidden again
        fx.property(dx2(idx["Shape"])).setValue(2);
        fx.property(dx2(idx["Shape"])).setValue(1);
        comp.openInViewer();
        T.selected = true;
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
