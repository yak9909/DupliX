// Lists every DupliX effect's parameters (name, match name, value) in a project: DUPLIX_USER_AEP (open a COPY: AE
// saves a modified project when the test closes it). Checks a project converted to a new match name keeps its
// values. Log: DUPLIX_TEST_DIR/jsx.log
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    try {
        app.open(new File($.getenv("DUPLIX_USER_AEP")));
        var P = app.project, n = 0;
        for (var i = 1; i <= P.numItems; i++) {
            var c = P.item(i);
            if (!(c instanceof CompItem)) continue;
            for (var j = 1; j <= c.numLayers; j++) {
                var fxs = c.layer(j).property("ADBE Effect Parade");
                if (!fxs) continue;
                for (var k = 1; k <= fxs.numProperties; k++) {
                    var fx = fxs.property(k);
                    if (fx.matchName !== "DupliX") continue;
                    n++;
                    log("effect in comp '" + c.name + "' layer '" + c.layer(j).name + "': " + fx.numProperties + " properties");
                    for (var q = 1; q <= fx.numProperties; q++) {
                        var p = fx.property(q), v = "";
                        try { v = p.value; } catch (e1) { v = "(group)"; }
                        log("  " + q + " " + p.matchName + " [" + p.name + "] = " + v);
                    }
                }
            }
        }
        log("effects " + n);
        P.close(CloseOptions.DO_NOT_SAVE_CHANGES);
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.toString() + " line " + e.line);
    }
})();
