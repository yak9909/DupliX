// P0 (design v4 2: "Count" on a macro with keys): DupliX's Copies keyframed in 3D Layers. The count goes 3 -> 12
// over frames 0..30, then drops to 2 at frame 31. For Optimize off and on (o0 / o1):
//   key_<o>_<f>.png   the keyed comp saved frame after frame in one session (the plugin's caches carry over)
//   ref_<o>_<f>.png   a comp with Copies fixed at that frame's value, each saved once (no history)
// The project is saved as count_keys.aep with render queue items for key_o0 / key_o1 (frames 0..40, TIFF), which
// tests/ae/run_count_keys.ps1 renders with aerender (MFR on). tests/ae/check_count_keys.py compares.
// Env: DUPLIX_TEST_DIR.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var FRAMES = [0, 5, 10, 20, 29, 30, 31, 35, 40];
    function png(comp, f, name) {
        var file = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(f / 30, file);
        for (var k = 0; k < 100 && !file.exists; k++) $.sleep(100);
        if (!file.exists) log("png " + name + " MISSING");
    }
    function build(name, opt) {
        var comp = app.project.items.addComp(name, 400, 300, 1, 2, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", 400, 300, 1);
        comp.layers.addCamera("Cam", [200, 150]);
        var T = comp.layers.addSolid([0.9, 0.7, 0.2], "Target", 40, 40, 1);
        T.threeDLayer = true;
        tg(T).property("ADBE Position").setValue([200, 150, 0]);
        var e = T.property("ADBE Effect Parade").addProperty("DupliX");
        e.property(dx(37)).setValue(1);  // 3D Layers
        e.property(dx(27)).setValue(opt);  // Optimize
        e.property(dx(3)).setValue(5);
        for (var k = 0; k < 3; k++) e.property(dx(4 + k)).setValue(k < 2 ? 300 : 200);
        e.property(dx(9)).setValue(90);
        return {comp: comp, fx: e};
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        for (var o = 0; o <= 1; o++) {
            var K = build("key_o" + o, o);
            var cp = K.fx.property(dx(2));
            cp.setValueAtTime(0, 3);
            cp.setValueAtTime(1, 12);
            cp.setValueAtTime(31 / 30, 2);
            cp.setInterpolationTypeAtKey(3, KeyframeInterpolationType.HOLD);
            var vals = [];
            for (var i = 0; i < FRAMES.length; i++) {
                var f = FRAMES[i], v = cp.valueAtTime(f / 30, false);
                vals.push(f + ":" + v);
                png(K.comp, f, "key_o" + o + "_" + f);
                var R = build("ref_o" + o + "_" + f, o);
                R.fx.property(dx(2)).setValue(v);
                png(R.comp, f, "ref_o" + o + "_" + f);
            }
            log("o" + o + " counts " + vals.join(" "));
            var rq = app.project.renderQueue.items.add(K.comp);
            var om = rq.outputModule(1);
            var t = om.templates, tpl = null;
            for (var ti = 0; ti < t.length; ti++) if (/TIFF/i.test(t[ti])) { tpl = t[ti]; break; }
            om.applyTemplate(tpl);
            om.file = new File(outDir + "/mfr_o" + o + "_[#####].tif");
            rq.timeSpanStart = 0;
            rq.timeSpanDuration = 41 / 30;
        }
        app.project.save(new File(outDir + "/count_keys.aep"));
        log("done");
    } catch (ex) {
        var msg = "?";
        try { msg = String(ex.message) + " (line " + String(ex.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
