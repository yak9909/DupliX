// Do the copies appear with every layer quality / sampling setting? For each setting: a 3D white
// solid with DupliX (10 copies) -> q_<name>.png. check: white pixels clearly more than one layer.
function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }  // a DupliX parameter by its disk ID
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function make(name, setup) {
        var comp = app.project.items.addComp(name, 400, 300, 1, 1, 30);
        comp.layers.addSolid([0.1, 0.1, 0.1], "BG", 400, 300, 1);
        comp.layers.addCamera("Cam", [200, 150]);
        var L = comp.layers.addSolid([1, 1, 1], "Target", 40, 40, 1);
        L.threeDLayer = true;
        var fx = L.property("ADBE Effect Parade").addProperty("DupliX");
        fx.property("DupliX-0002").setValue(10);
        fx.property("DupliX-0003").setValue(4);
        for (var q = 4; q <= 6; q++) fx.property(dx(q)).setValue(120);
        setup(L);
        var f = new File(outDir + "/q_" + name + ".png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " " + f.exists);
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        make("best_bicubic", function (L) {});
        make("best_bilinear", function (L) { L.samplingQuality = LayerSamplingQuality.BILINEAR; });
        make("draft", function (L) { L.quality = LayerQuality.DRAFT; });
        make("wireframe", function (L) { L.quality = LayerQuality.WIREFRAME; });
        log("done");
    } catch (e) { log("EXCEPTION " + e.message + " (line " + e.line + ")"); }
})();
