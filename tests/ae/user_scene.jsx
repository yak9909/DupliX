// Inspects a user-provided project and times frame renders under variants.
// Env: DUPLIX_TEST_DIR (output), DUPLIX_USER_AEP (a COPY of the user's project: run_user.ps1 makes it.
// AE saves a modified project on close, so the user's file itself must never be opened).
// Writes jsx.log: project/comp/layer/footage info, then "time <variant> <ms per frame>", "done".
function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }  // a DupliX parameter by its disk ID
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
    // ms per frame over `n` frames (cache purged first; the PNG write is included and the same for all)
    function timeFrames(comp, name, n) {
        app.purge(PurgeTarget.ALL_CACHES);
        var f = new File(outDir + "/" + name + ".png");
        var t0 = new Date().getTime();
        for (var i = 0; i < n; i++) {
            comp.saveFrameToPng(i * comp.frameDuration, f);
        }
        var ms = (new Date().getTime() - t0) / n;
        log("time " + name + " " + ms.toFixed(1) + " ms/frame");
        return ms;
    }
    try {
        app.beginSuppressDialogs();
        app.open(new File($.getenv("DUPLIX_USER_AEP")));
        var P = app.project;
        log("bpc=" + P.bitsPerChannel + " gpu=" + P.gpuAccelType + " items=" + P.numItems);
        // DUPLIX_USER_GPU (optional): override the project's GPU acceleration (SOFTWARE, CUDA, ...)
        var gpuOverride = $.getenv("DUPLIX_USER_GPU");
        if (gpuOverride) {
            P.gpuAccelType = GpuAccelType[gpuOverride];
            log("gpu set: " + P.gpuAccelType);
        }
        var main = null;
        for (var i = 1; i <= P.numItems; i++) {
            var it = P.item(i);
            if (it instanceof CompItem) {
                var info = "comp '" + it.name + "'";
                var keys = ["width", "height", "frameRate", "duration", "renderer", "numLayers", "resolutionFactor"];
                for (var kk = 0; kk < keys.length; kk++) {
                    try { info += " " + keys[kk] + "=" + it[keys[kk]]; } catch (ek) { info += " " + keys[kk] + "=(err)"; }
                }
                log(info);
                for (var j = 1; j <= it.numLayers; j++) {
                    var L = it.layer(j);
                    var src = L.source ? (L.source.name + " " + L.source.width + "x" + L.source.height) : "-";
                    var fx = fxOf(L);
                    var fxs = "";
                    if (fx) {
                        main = {comp: it, layer: L};
                        for (var k = 1; k <= fx.numProperties; k++) { try { fxs += " " + fx.property(k).value; } catch (e0) {} }
                    }
                    var par = L.property("ADBE Effect Parade");
                    log("  layer " + j + " '" + L.name + "' 3d=" + L.threeDLayer + " src=" + src + " effects=" +
                        (par ? par.numProperties : "-") + (fx ? " DupliX:" + fxs : "") +
                        " quality=" + L.quality + " mb=" + L.motionBlur);
                }
            } else if (it instanceof FootageItem) {
                log("footage '" + it.name + "' " + it.width + "x" + it.height + " file=" +
                    (it.file ? it.file.fsName : "-"));
            }
        }
        if (!main) throw new Error("no layer with the effect");
        // Variant comps (rendered by run_user.ps1 with aerender). Each is a duplicate of the user's comp.
        function variant(name, fn) {
            var c = main.comp.duplicate();
            c.name = name;
            var T = null;
            for (var a = 1; a <= c.numLayers; a++) if (fxOf(c.layer(a))) T = c.layer(a);
            fn(c, T, fxOf(T));
            // every frame different (AE reuses identical frames): move the camera a bit
            for (var b = 1; b <= c.numLayers; b++)
                if (c.layer(b) instanceof CameraLayer)
                    tg(c.layer(b)).property("ADBE Position").expression = "value + [time * 40, 0, 0]";
            log("variant " + name);
        }
        variant("v_asis", function () {});
        variant("v_count1", function (c, T, f) { f.property(dx(2)).setValue(1); });
        variant("v_count0", function (c, T, f) { f.property(dx(2)).setValue(0); });
        variant("v_plain", function (c, T, f) { f.remove(); });
        // 10 real layers (no effect), spread over the same kind of range
        variant("v_real10", function (c, T, f) {
            f.remove();
            for (var d = 0; d < 10; d++) {
                var D = T.duplicate();
                var p = tg(D).property("ADBE Position").value;
                tg(D).property("ADBE Position").setValue([p[0] + (d - 5) * 150, p[1] + ((d * 53) % 400) - 200, p[2] + ((d * 71) % 400) - 200]);
            }
            T.enabled = false;
        });
        // the same with the effect on each real layer (count 1, ranges 0): the cost of the effect itself
        variant("v_real10_fx", function (c, T, f) {
            f.property(dx(2)).setValue(1);
            for (var q = 4; q <= 9; q++) f.property(dx(q)).setValue(0);
            for (var d = 0; d < 10; d++) {
                var D = T.duplicate();
                var p = tg(D).property("ADBE Position").value;
                tg(D).property("ADBE Position").setValue([p[0] + (d - 5) * 150, p[1] + ((d * 53) % 400) - 200, p[2] + ((d * 71) % 400) - 200]);
            }
            T.enabled = false;
        });
        // fast mode (property 15 = RP_FAST) with 10 / 100 / 1000 copies (other parameters as the user set them)
        variant("v_fast10", function (c, T, f) { f.property(dx(2)).setValue(10); f.property("DupliX-0037").setValue(2); });
        variant("v_fast100", function (c, T, f) { f.property(dx(2)).setValue(100); f.property("DupliX-0037").setValue(2); });
        variant("v_fast1000", function (c, T, f) { f.property(dx(2)).setValue(1000); f.property("DupliX-0037").setValue(2); });
        // normal mode with and without merging (the merge checkbox: disk ID RD_MERGE = 27)
        var MERGE = "DupliX-0027";
        variant("v_norm100", function (c, T, f) { f.property(dx(2)).setValue(100); });
        variant("v_merge10", function (c, T, f) { f.property(MERGE).setValue(1); });
        variant("v_merge100", function (c, T, f) { f.property(dx(2)).setValue(100); f.property(MERGE).setValue(1); });
        variant("v_merge1000", function (c, T, f) { f.property(dx(2)).setValue(1000); f.property(MERGE).setValue(1); });
        // probe: the layer in draft quality (nearest sampling) with merging - the group plane is pixel-aligned
        variant("v_merge10_draft", function (c, T, f) { f.property(MERGE).setValue(1); T.quality = LayerQuality.DRAFT; });
        variant("v_merge100_draft", function (c, T, f) { f.property(dx(2)).setValue(100); f.property(MERGE).setValue(1); T.quality = LayerQuality.DRAFT; });
        app.project.save(new File(outDir + "/variants.aep"));
        log("done");
    } catch (ex) {
        log("EXCEPTION " + ex.message + " (line " + ex.line + ")");
    }
})();
