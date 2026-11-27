// Renders a COPY of the user's project as it is: the comp with the effect, duplicated into as_fm (every
// DupliX in fast mode) and as_nm (every DupliX in normal mode), at the comp's current time.
// aerender output: DUPLIX_RENDER_DIR/r_as_{fm,nm}_[#####].tif; viewer path: ui_{full,half}_{fm,nm}.png.
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
        app.open(new File($.getenv("DUPLIX_USER_AEP")));
        var P = app.project, main = null;
        for (var i = 1; i <= P.numItems && !main; i++) {
            var it = P.item(i);
            if (!(it instanceof CompItem)) continue;
            for (var j = 1; j <= it.numLayers; j++) if (fxOf(it.layer(j))) main = it;
        }
        if (!main) throw new Error("no comp with the effect");
        var t = 0;
        try { t = main.time; } catch (et) { log("comp.time unreadable: " + et.message); }
        log("comp " + main.width + "x" + main.height + " time=" + t + " layers=" + main.numLayers);
        for (var li = 1; li <= main.numLayers; li++) {
            var Ly = main.layer(li), fx = fxOf(Ly), v = "";
            if (fx) for (var k = 2; k <= fx.numProperties; k++) { try { v += " " + fx.property(k).value; } catch (e0) {} }
            log("layer " + li + " 3d=" + Ly.threeDLayer + " quality=" + Ly.quality + " sampling=" + Ly.samplingQuality +
                (fx ? " fx:" + v : ""));
        }
        while (P.renderQueue.numItems > 0) P.renderQueue.item(1).remove();
        function make(name, fast) {
            var c = main.duplicate();
            c.name = name;
            for (var a = 1; a <= c.numLayers; a++) { var f = fxOf(c.layer(a)); if (f) f.property("DupliX-0037").setValue(fast ? 2 : 1); }
            var rq = P.renderQueue.items.add(c);
            var om = rq.outputModule(1), tl = om.templates, tpl = null;
            for (var q = 0; q < tl.length; q++) if (/TIFF/i.test(tl[q]) && tl[q] !== "RNDM_TIFF_A") { tpl = tl[q]; break; }
            om.applyTemplate(tpl);
            om.file = new File($.getenv("DUPLIX_RENDER_DIR") + "/r_" + name + "_[#####].tif");
            rq.timeSpanStart = t;
            rq.timeSpanDuration = c.frameDuration;
            return c;
        }
        var fm = make("as_fm", true), nm = make("as_nm", false);
        function png(c, name, res) {
            c.resolutionFactor = res;
            var f = new File(outDir + "/" + name + ".png");
            c.saveFrameToPng(t, f);
            for (var k2 = 0; k2 < 100 && !f.exists; k2++) $.sleep(100);
            log("png " + name + " exists=" + f.exists);
        }
        png(fm, "ui_full_fm", [1, 1]); png(nm, "ui_full_nm", [1, 1]);
        png(fm, "ui_half_fm", [2, 2]); png(nm, "ui_half_nm", [2, 2]);
        fm.resolutionFactor = [1, 1]; nm.resolutionFactor = [1, 1];
        P.save(new File(outDir + "/as_is.aep"));
        log("done");
    } catch (ex) {
        log("EXCEPTION " + ex.message + " (line " + ex.line + ")");
    }
})();
