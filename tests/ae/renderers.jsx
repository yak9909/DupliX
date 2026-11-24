// Probe: do the copies appear with each 3D renderer of the comp (Classic 3D / Advanced 3D / ...)?
// For every renderer in comp.renderers: probe_<i> (Target + randamu, 20 copies) and ref_<i> (Target
// without the effect), PNG each, and log the renderer names. Writes jsx.log ("done").
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function png(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        var t0 = new Date().getTime();
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 300 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists + " ms=" + (new Date().getTime() - t0));
    }
    function scene(name, renderer, withFx, count) {
        var comp = app.project.items.addComp(name, 400, 300, 1, 2, 30);
        comp.renderer = renderer;
        comp.layers.addSolid([0.1, 0.1, 0.1], "BG", 400, 300, 1);
        var wall = comp.layers.addSolid([0.2, 0.3, 1], "Wall", 400, 300, 1);
        wall.threeDLayer = true;
        tg(wall).property("ADBE Position").setValue([240, 140, 0]);
        tg(wall).property("ADBE Rotate Y").setValue(90);
        comp.layers.addCamera("Cam", [200, 150]);
        var L = comp.layers.addSolid([1, 1, 1], "Target", 60, 60, 1);
        L.threeDLayer = true;
        if (withFx) {
            var fx = L.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
            fx.property(2).setValue(count);
            fx.property(3).setValue(3);
            for (var q = 4; q <= 6; q++) fx.property(q).setValue(150);
            for (var r = 7; r <= 9; r++) fx.property(r).setValue(60);
        }
        return comp;
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var tmp = app.project.items.addComp("tmp", 100, 100, 1, 1, 30);
        var rs = tmp.renderers;
        log("renderers: " + rs.join(", ") + " default=" + tmp.renderer);
        for (var i = 0; i < rs.length; i++) {
            try {
                png(scene("probe_" + i, rs[i], true, 20), "probe_" + i);
                png(scene("ref_" + i, rs[i], false, 0), "ref_" + i);
                log("renderer " + i + " = " + rs[i] + " ok");
            } catch (e1) { log("renderer " + i + " = " + rs[i] + " error " + e1.message); }
        }
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.message + " (line " + e.line + ")");
    }
})();
