// Smoke test: DupliX loads, can be applied to a 3D solid, and turns it into copies.
// one.png: count 1, all ranges 0 (= the plain layer). many.png: count 10, position range 120.
// Writes jsx.log (parameter names, TARGET_ID/COMP_ID, "SMOKE OK") to $DUPLIX_TEST_DIR.
function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }  // a DupliX parameter by its disk ID
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
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var comp = app.project.items.addComp("smoke", 400, 300, 1, 2, 30);
        comp.layers.addCamera("Cam", [200, 150]);
        var L = comp.layers.addSolid([1, 1, 1], "Target", 40, 40, 1);
        L.threeDLayer = true;
        log("before addProperty");
        var fx = L.property("ADBE Effect Parade").addProperty("DupliX");
        log("effect " + (fx ? fx.matchName : "null") + " params=" + fx.numProperties);
        for (var i = 1; i <= fx.numProperties; i++) {
            var p = fx.property(i);
            var v = "";
            try { v = p.value; } catch (e1) { v = "(n/a)"; }
            log(" #" + i + " " + p.matchName + " [" + p.name + "] = " + v);
        }
        log("TARGET_ID=" + L.id + " COMP_ID=" + comp.id);
        fx.property("DupliX-0002").setValue(1);                          // count
        for (var r = 4; r <= 9; r++) fx.property(dx(r)).setValue(0);  // position / rotation ranges
        png(comp, "one");
        fx.property("DupliX-0002").setValue(10);
        for (var q = 4; q <= 6; q++) fx.property(dx(q)).setValue(120);
        png(comp, "many");
        log("SMOKE OK");
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        var f2 = new File(outDir + "/jsx.log");
        f2.encoding = "UTF-8"; f2.open("a"); f2.writeln("EXCEPTION " + msg); f2.close();
    }
})();
