// Reproduction of the user's report (11/24): turning the camera's orbit null about Y changes the drawing
// order of the copies. Opens a COPY of the user's project (RANDAMU_USER_AEP), duplicates the comp with the
// effect into orb_fm (fast mode) and orb_nm (normal mode, = real duplicates), keys the orbit null's Y
// rotation 0 -> 360 over the comp's first 120 frames, queues both (TIFF r_<comp>_[#####].tif), saves orbit.aep.
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function fxOf(L) {
        var p = L.property("ADBE Effect Parade");
        if (!p) return null;
        for (var i = 1; i <= p.numProperties; i++) if (p.property(i).matchName === "RANDAMU Scatter") return p.property(i);
        return null;
    }
    var FRAMES = 120;
    try {
        app.beginSuppressDialogs();
        app.open(new File($.getenv("RANDAMU_USER_AEP")));
        var P = app.project, main = null;
        for (var i = 1; i <= P.numItems && !main; i++) {
            var it = P.item(i);
            if (!(it instanceof CompItem)) continue;
            for (var j = 1; j <= it.numLayers; j++) if (fxOf(it.layer(j))) main = it;
        }
        if (!main) throw new Error("no comp with the effect");
        function make(name, fast) {
            var c = main.duplicate();
            c.name = name;
            var cam = null, T = null;
            for (var a = 1; a <= c.numLayers; a++) {
                if (c.layer(a) instanceof CameraLayer) cam = c.layer(a);
                if (fxOf(c.layer(a))) T = c.layer(a);
            }
            if (!cam || !cam.parent) throw new Error("camera without an orbit null");
            var ry = tg(cam.parent).property("ADBE Rotate Y");
            ry.setValueAtTime(0, 0);
            ry.setValueAtTime(FRAMES * c.frameDuration, 360);
            fxOf(T).property(15).setValue(fast ? 1 : 0);
            log(name + ": camera '" + cam.name + "' parent '" + cam.parent.name + "' count=" + fxOf(T).property(2).value);
            var rq = P.renderQueue.items.add(c);
            var om = rq.outputModule(1);
            // the user's project may carry its own templates: pick a TIFF one by name, not RNDM_TIFF_A
            var t = om.templates, tpl = null;
            for (var k = 0; k < t.length; k++) if (/TIFF/i.test(t[k]) && t[k] !== "RNDM_TIFF_A") { tpl = t[k]; break; }
            om.applyTemplate(tpl);
            log(name + ": output template '" + tpl + "'");
            // renders go to RANDAMU_RENDER_DIR (2 x 120 full-HD TIFFs ~ 2 GB: kept off the nearly full A: drive)
            om.file = new File(($.getenv("RANDAMU_RENDER_DIR") || outDir) + "/r_" + name + "_[#####].tif");
            rq.timeSpanStart = 0;
            rq.timeSpanDuration = FRAMES * c.frameDuration;
        }
        // keep only our two items in the queue
        while (P.renderQueue.numItems > 0) P.renderQueue.item(1).remove();
        make("orb_fm", true);
        make("orb_nm", false);
        P.save(new File(outDir + "/orbit.aep"));
        log("done");
    } catch (ex) {
        log("EXCEPTION " + ex.message + " (line " + ex.line + ")");
    }
})();
