// Times a user's scene (run_user_time.ps1): opens the COPY named by RANDAMU_USER_AEP and, for every comp holding a
// DupliX layer, renders RANDAMU_UT_FRAMES consecutive frames from RANDAMU_UT_START (MFR off) under each variant in
// RANDAMU_UT_VARIANTS: asis (as saved), on / off (merge on / off on every DupliX layer), none (every DupliX layer
// off: the scene without copies). Logs "time <variant> <ms per frame>" (wall time of the render / frames).
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    var MERGE = "RANDAMU Scatter-0027";
    function dupli(L) {
        var p = L.property("ADBE Effect Parade");
        if (!p) return null;
        for (var i = 1; i <= p.numProperties; i++) if (p.property(i).matchName === "RANDAMU Scatter") return p.property(i);
        return null;
    }
    try {
        app.beginSuppressDialogs();
        try { app.setMultiFrameRenderingConfig(false, 100); log("mfr off"); } catch (em) { log("mfr: " + em.message); }
        app.open(new File($.getenv("RANDAMU_USER_AEP")));
        var P = app.project;
        var n = parseInt($.getenv("RANDAMU_UT_FRAMES") || "30", 10), start = parseInt($.getenv("RANDAMU_UT_START") || "0", 10);
        var variants = ($.getenv("RANDAMU_UT_VARIANTS") || "asis,on,off,none").split(",");
        var comps = [];
        for (var i = 1; i <= P.numItems; i++) {
            var it = P.item(i);
            if (!(it instanceof CompItem)) continue;
            for (var j = 1; j <= it.numLayers; j++) if (dupli(it.layer(j))) { comps.push(it); break; }
        }
        for (var c = 0; c < comps.length; c++) {
            var src0 = comps[c];
            for (var v = 0; v < variants.length; v++) {
                var vn = variants[v], cc = src0.duplicate();
                cc.name = "ut_" + vn;
                var desc = [];
                for (var j2 = 1; j2 <= cc.numLayers; j2++) {
                    var L = cc.layer(j2), d = dupli(L);
                    if (!d) continue;
                    if (vn === "on" || vn === "off") d.property(MERGE).setValue(vn === "on" ? 1 : 0);
                    if (vn === "none") L.enabled = false;
                    desc.push(L.name + (L.enabled ? "" : "(off)") + " count=" + d.property(2).value + " merge=" + d.property(MERGE).value);
                }
                log("comp " + src0.name + " variant " + vn + ": " + desc.join(", "));
                var tag = src0.id + "_" + vn;
                $.setenv("RANDAMU_LOG", (outDir + "/plugin_" + tag + ".log").replace(/\//g, "\\"));
                $.setenv("RANDAMU_CACHE_SALT", "ut-" + tag + "-" + new Date().getTime());
                app.purge(PurgeTarget.ALL_CACHES);
                var rq = P.renderQueue.items.add(cc);
                var om = rq.outputModule(1);
                var ok = false;
                try { om.applyTemplate("RNDM_TIFF_A"); ok = true; } catch (et) {}
                if (!ok) {
                    var t = om.templates, tpl = null;
                    for (var ti = 0; ti < t.length; ti++) if (/TIFF/i.test(t[ti])) { tpl = t[ti]; break; }
                    om.applyTemplate(tpl);
                    om.saveAsTemplate("RNDM_TIFF_A");
                }
                om.file = new File(outDir + "/r_" + tag + "_[#####].tif");
                rq.timeSpanStart = start / src0.frameRate;
                rq.timeSpanDuration = n / src0.frameRate;
                for (var qi = 1; qi < P.renderQueue.numItems; qi++) {
                    var qq = P.renderQueue.item(qi);
                    if (qq.status === RQItemStatus.QUEUED) qq.render = false;
                }
                var t0 = new Date().getTime();
                P.renderQueue.render();
                var ms = new Date().getTime() - t0;
                log("time " + vn + " " + (ms / n).toFixed(1) + " ms/frame (" + n + " frames, " + ms + " ms)");
            }
        }
        $.setenv("RANDAMU_LOG", "");
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.message + " (line " + e.line + ")");
    }
})();
