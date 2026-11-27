// One-off: undoes what an earlier version of user_scene.jsx saved into the user's project by mistake:
// it added 10 duplicate layers (each with DupliX count 1), hid the original layer and switched its
// DupliX off. Opens a COPY ($DUPLIX_RESTORE_IN) and saves the result as $DUPLIX_TEST_DIR/restored.aep.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function fxOf(L) {
        var p = L.property("ADBE Effect Parade");
        if (!p) return null;
        for (var i = 1; i <= p.numProperties; i++) if (p.property(i).matchName === "DupliX") return p.property(i);
        return null;
    }
    try {
        app.beginSuppressDialogs();
        app.open(new File($.getenv("DUPLIX_RESTORE_IN")));
        var P = app.project, comp = null;
        for (var i = 1; i <= P.numItems; i++) if (P.item(i) instanceof CompItem) { comp = P.item(i); log("comp " + comp.name + " layers=" + comp.numLayers); }
        var orig = null, removed = 0;
        for (var j = comp.numLayers; j >= 1; j--) {
            var L = comp.layer(j), f = fxOf(L);
            if (!f) continue;
            if (f.property("DupliX-0002").value === 1) { L.remove(); removed++; }
            else orig = L;
        }
        if (!orig || removed !== 10) throw new Error("unexpected state: removed=" + removed + " orig=" + (orig ? orig.name : "none"));
        orig.enabled = true;
        fxOf(orig).enabled = true;
        log("removed " + removed + "; original '" + orig.name + "' enabled, effect on, count=" + fxOf(orig).property("DupliX-0002").value);
        for (var k = 1; k <= comp.numLayers; k++) log("  layer " + k + " '" + comp.layer(k).name + "' enabled=" + comp.layer(k).enabled);
        log("items=" + P.numItems);
        P.save(new File(outDir + "/restored.aep"));
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.message + " (line " + e.line + ")");
    }
})();
