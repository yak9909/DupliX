// THROWAWAY spike (merge optimisation, design 12.7 (3)): can a sub-layer's rect pick a part of the effect's
// output? Builds atlas.aep: a PNG target (3D) with DupliX (2 copies, normal mode) under a camera, and the same
// with a plain solid backdrop; one render queue item per comp. The spike behaviour itself is switched by
// DUPLIX_SPIKE_ATLAS in the plugin at render time (tests/ae/run_spike_atlas.ps1).
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function addRQ(comp) {
        var rq = app.project.renderQueue.items.add(comp);
        var om = rq.outputModule(1);
        var ok = false;
        try { om.applyTemplate("RNDM_TIFF_A"); ok = true; } catch (et) {}
        if (!ok) {
            var t = om.templates, tpl = null;
            for (var i = 0; i < t.length; i++) if (/TIFF/i.test(t[i])) { tpl = t[i]; break; }
            om.applyTemplate(tpl);
            om.saveAsTemplate("RNDM_TIFF_A");
        }
        om.file = new File(outDir + "/r_" + comp.name + "_[#####].tif");
        rq.timeSpanStart = 0;
        rq.timeSpanDuration = 1 / 30;
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var img = app.project.importFile(new ImportOptions(new File($.getenv("DUPLIX_SPIKE_IMAGE"))));
        var comp = app.project.items.addComp("atlas", 400, 300, 1, 1, 30);
        comp.layers.addSolid([0.1, 0.1, 0.1], "BG", 400, 300, 1);
        var T = comp.layers.add(img);
        T.name = "Target";
        T.threeDLayer = true;
        tg(T).property("ADBE Position").setValue([200, 150, 0]);
        var fx = T.property("ADBE Effect Parade").addProperty("DupliX");
        fx.property(2).setValue(2);                       // count
        fx.property(3).setValue(3);                       // seed
        fx.property(4).setValue(200);                     // position range X
        fx.property(5).setValue(60);                      // Y
        if ($.getenv("DUPLIX_SPIKE_TINY")) {             // far and small copies: strong minification
            fx.property(2).setValue(60);
            fx.property(6).setValue(3000);
            for (var k2 = 7; k2 <= 9; k2++) fx.property(k2).setValue(80);
            fx.property(10).setValue(5);
            fx.property(11).setValue(20);
        } else if ($.getenv("DUPLIX_SPIKE_ROT")) {              // rotation range XYZ 60, scale 50..150, depth 300
            fx.property(6).setValue(300);
            for (var k = 7; k <= 9; k++) fx.property(k).setValue(60);
            fx.property(10).setValue(50);
            fx.property(11).setValue(150);
        }
        comp.layers.addCamera("Cam", [200, 150]);
        addRQ(comp);
        log("built TARGET_ID=" + T.id + " COMP_ID=" + comp.id);
        app.project.save(new File(outDir + "/atlas.aep"));
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.message + " (line " + e.line + ")");
    }
})();
