// P0 (design v4 2, Layer Alpha): spike/P0Graph reads its "Alpha Layer" at the Sample Time and rebuilds its point set
// only when that layer's alpha changes. The alpha layer is a precomp ("Src": a solid with a mask) used hidden.
// A layer parameter gives the layer's source (11/28: a mask on the layer itself was not in it), so the changes are
// made inside the precomp. Steps, each followed by a render at 1 s (a1..a5.png); the plugin log says
// "alpha: rebuilt" or "alpha: reused" for each:
//   1 the first render (rebuilt)       2 Macro 1 changed (reused: the alpha is the same)
//   3 the shape inside changed (rebuilt)   4 its color changed (reused)
//   5 the shape changed after the Sample Time only (reused, if AE renders at all)
// Env: DUPLIX_TEST_DIR, DUPLIX_P0_SAMPLE (the Sample Time, s; default 0).
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function square(a, b) {
        var s = new Shape();
        s.vertices = [[a, a], [b, a], [b, b], [a, b]];
        s.closed = true;
        return s;
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var src = app.project.items.addComp("Src", 200, 200, 1, 4, 30);
        var inner = src.layers.addSolid([1, 1, 1], "Inner", 200, 200, 1);
        var path = inner.property("ADBE Mask Parade").addProperty("ADBE Mask Atom").property("ADBE Mask Shape");
        path.setValueAtTime(0, square(20, 180));
        var c = app.project.items.addComp("alpha", 200, 200, 1, 4, 30);
        var B = c.layers.add(src);
        B.enabled = false;
        var A = c.layers.addSolid([0, 0, 1], "S", 200, 200, 1);
        var e = A.property("ADBE Effect Parade").addProperty("DupliXP0");
        e.property("DupliXP0-0005").setValue(B.index);
        var sample = parseFloat($.getenv("DUPLIX_P0_SAMPLE") || "0");
        e.property("DupliXP0-0006").setValue(sample);
        log("sample time " + sample);
        var k = 0;
        function render(what) {
            k++;
            var f = new File(outDir + "/a" + k + ".png");
            c.saveFrameToPng(1, f);
            for (var j = 0; j < 100 && !f.exists; j++) $.sleep(100);
            log("step " + k + " " + what + ": png " + f.exists);
        }
        render("first");
        e.property("DupliXP0-0003").setValue(12);
        render("Macro 1 changed");
        path.setValueAtTime(0, square(60, 140));
        render("shape changed");
        inner.property("ADBE Effect Parade").addProperty("ADBE Fill").property("ADBE Fill-0002").setValue([1, 0, 0]);
        render("color changed");
        path.setValueAtTime(3, square(20, 180));
        render("shape changed after the Sample Time");
        log("done");
    } catch (ex) {
        var msg = "?";
        try { msg = String(ex.message) + " (line " + String(ex.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
