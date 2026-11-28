// DupliX 2: how long a frame takes (the user, 11/28: "Fast (2D) takes over a second"). 1920x1080 comps at 30 fps
// with the default 5 x 5 grid on: a 1920x1080 solid, a 200x200 solid; each frame rendered 3 times (t = 0, 1/30,
// 2/30: the first may include AE's own set-up). The effect logs the time of each PreRender and SmartRender.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        var sizes = [[1920, 1080], [200, 200]];
        for (var i = 0; i < sizes.length; i++) {
            var w = sizes[i][0], h = sizes[i][1];
            var c = app.project.items.addComp("P" + w, 1920, 1080, 1, 1, 30);
            var s = c.layers.addSolid([1, 0.5, 0], "S" + w, w, h, 1, 1);
            s.property("ADBE Effect Parade").addProperty("DupliX2");
            for (var k = 0; k < 3; k++) {
                var f = new File(outDir + "/p" + w + "_" + k + ".png");
                var t0 = new Date().getTime();
                c.saveFrameToPng(k / 30, f);
                for (var n = 0; n < 200 && !f.exists; n++) $.sleep(50);
                log("layer " + w + "x" + h + " frame " + k + ": " + (new Date().getTime() - t0) + " ms (png " + f.exists + ")");
            }
        }
        log("done");
    } catch (e) {
        log("EXCEPTION " + String(e.message) + " (line " + String(e.line) + ")");
    }
})();
