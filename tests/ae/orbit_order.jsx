// Reproduction of the user's report (11/24): turning the camera's orbit null about Y changes the drawing
// order of the copies. Opens a COPY of the user's project (DUPLIX_USER_AEP), duplicates the comp with the
// effect into orb_fm (fast mode) and orb_nm (normal mode, = real duplicates), keys the orbit null's Y
// rotation 0 -> 360 over the comp's first 120 frames, queues both (TIFF r_<comp>_[#####].tif), saves orbit.aep.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function fxOf(L) {
        var p = L.property("ADBE Effect Parade");
        if (!p) return null;
        for (var i = 1; i <= p.numProperties; i++) if (p.property(i).matchName === "DupliX") return p.property(i);
        return null;
    }
    var FRAMES = 120;
    try {
        app.beginSuppressDialogs();
        app.open(new File($.getenv("DUPLIX_USER_AEP")));
        var P = app.project, main = null;
        for (var i = 1; i <= P.numItems && !main; i++) {
            var it = P.item(i);
            if (!(it instanceof CompItem)) continue;
            for (var j = 1; j <= it.numLayers; j++) if (fxOf(it.layer(j))) main = it;
        }
        if (!main) throw new Error("no comp with the effect");
        for (var li = 1; li <= main.numLayers; li++) {
            var Ly = main.layer(li), srcInfo = "-";
            try {
                if (Ly.source) srcInfo = (Ly.source instanceof CompItem ? "comp(" + Ly.source.numLayers + " layers)" : "footage") +
                    " " + Ly.source.width + "x" + Ly.source.height;
            } catch (es) {}
            var fxl = fxOf(Ly), fxv = "";
            if (fxl) for (var pk = 2; pk <= fxl.numProperties; pk++) { try { fxv += " " + fxl.property(pk).value; } catch (e9) {} }
            log("layer " + li + " 3d=" + Ly.threeDLayer + " collapse=" + Ly.collapseTransformation + " src=" + srcInfo +
                (fxl ? " fx:" + fxv : ""));
            if (Ly.source instanceof CompItem)
                for (var sj = 1; sj <= Ly.source.numLayers; sj++)
                    log("   inner " + sj + " 3d=" + Ly.source.layer(sj).threeDLayer + " enabled=" + Ly.source.layer(sj).enabled);
        }
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
            var fixedDeg = $.getenv("DUPLIX_ORBIT_DEG");  // a fixed angle (one frame) instead of the sweep
            if (fixedDeg) {
                while (ry.numKeys > 0) ry.removeKey(1);
                ry.setValue(parseFloat(fixedDeg));
            } else {
                ry.setValueAtTime(0, 0);
                ry.setValueAtTime(FRAMES * c.frameDuration, 360);
            }
            fxOf(T).property("DupliX-0037").setValue(fast ? 2 : 1);
            log(name + ": camera '" + cam.name + "' parent '" + cam.parent.name + "' count=" + fxOf(T).property("DupliX-0002").value);
            var rq = P.renderQueue.items.add(c);
            var om = rq.outputModule(1);
            // the user's project may carry its own templates: pick a TIFF one by name, not RNDM_TIFF_A
            var t = om.templates, tpl = null;
            for (var k = 0; k < t.length; k++) if (/TIFF/i.test(t[k]) && t[k] !== "RNDM_TIFF_A") { tpl = t[k]; break; }
            om.applyTemplate(tpl);
            log(name + ": output template '" + tpl + "'");
            // renders go to DUPLIX_RENDER_DIR (2 x 120 full-HD TIFFs ~ 2 GB: kept off the nearly full A: drive)
            om.file = new File(($.getenv("DUPLIX_RENDER_DIR") || outDir) + "/r_" + name + "_[#####].tif");
            rq.timeSpanStart = 0;
            rq.timeSpanDuration = ($.getenv("DUPLIX_ORBIT_DEG") ? 1 : FRAMES) * c.frameDuration;
        }
        // keep only our two items in the queue
        while (P.renderQueue.numItems > 0) P.renderQueue.item(1).remove();
        make("orb_fm", true);
        make("orb_nm", false);
        if ($.getenv("DUPLIX_ORBIT_PNG") === "1") {
            // the viewer's path (saveFrameToPng), toggling fast mode on ONE comp like the user does
            var fm = null;
            for (var q = 1; q <= P.numItems; q++) if (P.item(q).name === "orb_fm") fm = P.item(q);
            var T2 = null;
            for (var r = 1; r <= fm.numLayers; r++) if (fxOf(fm.layer(r))) T2 = fm.layer(r);
            var fast = fxOf(T2).property("DupliX-0037");
            function png(name) {
                var f = new File(outDir + "/" + name + ".png");
                fm.saveFrameToPng(0, f);
                for (var k2 = 0; k2 < 100 && !f.exists; k2++) $.sleep(100);
                log("png " + name + " exists=" + f.exists);
            }
            var res = [[1, 1], [2, 2]];
            for (var s = 0; s < res.length; s++) {
                fm.resolutionFactor = res[s];
                var tag = "r" + res[s][0];
                fast.setValue(2); png("ui_" + tag + "_fast1");
                fast.setValue(1); png("ui_" + tag + "_normal");
                fast.setValue(2); png("ui_" + tag + "_fast2");
            }
            fm.resolutionFactor = [1, 1];
        }
        P.save(new File(outDir + "/orbit.aep"));
        log("done");
    } catch (ex) {
        log("EXCEPTION " + ex.message + " (line " + ex.line + ")");
    }
})();
