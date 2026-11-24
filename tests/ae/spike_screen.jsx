// SPIKE (run with RANDAMU_SPIKE_SCREEN=1): the effect outputs a pattern f(layer px) on a comp-sized
// plane that should land 1:1 on comp pixels. Saves s_<case>.png; check_spike_screen.py compares them
// with the pattern computed in Python.
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function make(name, w, h, camSetup, layerSetup) {
        var comp = app.project.items.addComp(name, w, h, 1, 1, 30);
        var cam = comp.layers.addCamera("Cam", [w / 2, h / 2]);
        camSetup(cam);
        var L = comp.layers.addSolid([1, 1, 1], "Target", 200, 150, 1);
        L.threeDLayer = true;
        layerSetup(comp, L);
        var fx = L.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
        fx.property(2).setValue(5);
        // render queue entry for aerender (TIFF, 1 frame)
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
        om.file = new File(outDir + "/r_" + name + "_[#####].tif");
        rq.timeSpanStart = 0;
        rq.timeSpanDuration = comp.frameDuration;
        if ($.getenv("RANDAMU_SPIKE_SAVEONLY") === "1") return;
        var f = new File(outDir + "/s_" + name + ".png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 200 && !f.exists; k++) $.sleep(100);
        log("png " + name + " " + f.exists);
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        make("plain", 1920, 1080, function (c) {}, function (comp, L) {});
        make("cam_moved", 1920, 1080, function (c) {
            tg(c).property("ADBE Position").setValue([700, 300, -1500]);
            tg(c).property("ADBE Anchor Point").setValue([1100, 600, 200]);
        }, function (comp, L) {
            tg(L).property("ADBE Position").setValue([900, 500, 300]);
            tg(L).property("ADBE Rotate X").setValue(25);
            tg(L).property("ADBE Rotate Y").setValue(-40);
            tg(L).property("ADBE Anchor Point").setValue([30, 20, 0]);
            tg(L).property("ADBE Scale").setValue([140, 80, 100]);
        });
        make("parented", 400, 300, function (c) {
            tg(c).property("ADBE Position").setValue([150, 100, -500]);
        }, function (comp, L) {
            var n = comp.layers.addNull();
            n.threeDLayer = true;
            tg(n).property("ADBE Position").setValue([220, 160, 50]);
            tg(n).property("ADBE Rotate Z").setValue(30);
            L.setParentWithJump(n);
            tg(L).property("ADBE Rotate Y").setValue(20);
        });
        app.project.save(new File(outDir + "/spike.aep"));
        log("done");
    } catch (e) { log("EXCEPTION " + e.message + " (line " + e.line + ")"); }
})();
