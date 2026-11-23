// Smoke test: randamu loads, can be applied to a 3D solid, and the comp renders.
// Writes jsx.log (parameter names, "SMOKE OK") and smoke.png to $RANDAMU_TEST_DIR.
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var comp = app.project.items.addComp("smoke", 400, 300, 1, 2, 30);
        comp.layers.addCamera("Cam", [200, 150]);
        var L = comp.layers.addSolid([1, 1, 1], "Target", 40, 40, 1);
        L.threeDLayer = true;
        var fx = L.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
        log("effect " + (fx ? fx.matchName : "null") + " params=" + fx.numProperties);
        for (var i = 1; i <= fx.numProperties; i++) {
            var p = fx.property(i);
            var v = "";
            try { v = p.value; } catch (e1) { v = "(n/a)"; }
            log(" #" + i + " " + p.matchName + " [" + p.name + "] = " + v);
        }
        log("TARGET_ID=" + L.id + " COMP_ID=" + comp.id);
        var f = new File(outDir + "/smoke.png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png exists=" + f.exists);
        log("SMOKE OK");
        log("done");
    } catch (e) {
        log("EXCEPTION " + e + " line " + e.line);
    }
})();
