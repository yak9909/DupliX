// A user's scene with the merge on and off (run_user_merge.ps1): opens the COPY named by DUPLIX_USER_AEP, logs
// its structure, and for every comp holding a DupliX layer renders DUPLIX_UM_FRAMES frames spread over the comp
// (or the frames listed in DUPLIX_UM_AT, comma separated) twice: every DupliX layer with merge on, then off.
// Outputs <comp id>_<on|off>_<frame>.tif and plugin_<comp id>_<on|off>.log in DUPLIX_TEST_DIR.
function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }  // a DupliX parameter by its disk ID
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    var MERGE = "DupliX-0027";
    function fxList(L) {
        var r = [], p = L.property("ADBE Effect Parade");
        if (!p) return r;
        for (var i = 1; i <= p.numProperties; i++) r.push(p.property(i));
        return r;
    }
    function dupli(L) {
        var f = fxList(L);
        for (var i = 0; i < f.length; i++) if (f[i].matchName === "DupliX") return f[i];
        return null;
    }
    try {
        app.beginSuppressDialogs();
        app.open(new File($.getenv("DUPLIX_USER_AEP")));
        var P = app.project;
        log("bpc=" + P.bitsPerChannel + " gpu=" + P.gpuAccelType);
        var comps = [];
        for (var i = 1; i <= P.numItems; i++) {
            var it = P.item(i);
            if (!(it instanceof CompItem)) continue;
            var has = false;
            log("comp " + it.id + " '" + it.name + "' " + it.width + "x" + it.height + " fps=" + it.frameRate +
                " dur=" + it.duration + " renderer=" + it.renderer + " layers=" + it.numLayers + " mb=" + it.motionBlur);
            for (var j = 1; j <= it.numLayers; j++) {
                var L = it.layer(j);
                var fx = fxList(L), names = [];
                for (var k = 0; k < fx.length; k++) names.push(fx[k].matchName + (fx[k].enabled ? "" : "(off)"));
                var d = dupli(L), dv = "";
                if (d) {
                    has = true;
                    for (var q = 1; q <= d.numProperties; q++) { try { dv += " " + d.property(q).value; } catch (e0) {} }
                }
                var src = L.source ? (L.source.name + " " + L.source.width + "x" + L.source.height) : "-";
                log("  " + j + " '" + L.name + "' " + (L instanceof CameraLayer ? "camera" : L instanceof LightLayer ? "light" :
                    L instanceof TextLayer ? "text" : L instanceof ShapeLayer ? "shape" : "av") +
                    " 3d=" + L.threeDLayer + " on=" + L.enabled + " src=" + src + " parent=" + (L.parent ? L.parent.index : "-") +
                    " blend=" + L.blendingMode + " matte=" + (L.hasTrackMatte ? L.trackMatteType : "-") +
                    " in=" + L.inPoint + " out=" + L.outPoint + " fx=[" + names.join(",") + "]" + (d ? " dupli:" + dv : ""));
            }
            if (has) comps.push(it);
        }
        var n = parseInt($.getenv("DUPLIX_UM_FRAMES") || "8", 10), at = $.getenv("DUPLIX_UM_AT") || "";
        for (var c = 0; c < comps.length; c++) {
            var src0 = comps[c], frames = [];
            var total = Math.round(src0.duration * src0.frameRate);
            if (at) { var a = at.split(","); for (var z = 0; z < a.length; z++) frames.push(parseInt(a[z], 10)); }
            else for (var z2 = 0; z2 < n; z2++) frames.push(Math.floor(z2 * (total - 1) / Math.max(1, n - 1)));
            var modes = ["on", "off"];
            for (var m = 0; m < modes.length; m++) {
                var cc = src0.duplicate();
                cc.name = "um_" + modes[m];
                // experiments: DUPLIX_UM_DISABLE (a layer name: turned off), DUPLIX_UM_MOVEZ ("name:dz": moved in z)
                var dis = $.getenv("DUPLIX_UM_DISABLE") || "", mv = $.getenv("DUPLIX_UM_MOVEZ") || "";
                for (var j3 = 1; j3 <= cc.numLayers; j3++) {
                    var L3 = cc.layer(j3);
                    if (dis && L3.name === dis) { L3.enabled = false; log("disabled " + dis); }
                    if (mv && L3.name === mv.split(":")[0]) {
                        var pp = L3.property("ADBE Transform Group").property("ADBE Position");
                        var v = pp.value;
                        pp.setValue([v[0], v[1], v[2] + parseFloat(mv.split(":")[1])]);
                        log("moved " + L3.name + " z by " + mv.split(":")[1]);
                    }
                }
                for (var j2 = 1; j2 <= cc.numLayers; j2++) {
                    var d2 = dupli(cc.layer(j2));
                    if (d2) d2.property(MERGE).setValue(modes[m] === "on" ? 1 : 0);
                }
                // DUPLIX_UM_SET ("layer:index=value;..."): a DupliX parameter of a named layer set (e.g. the world
                // rotation Y, index 21)
                var sets = ($.getenv("DUPLIX_UM_SET") || "").split(";");
                for (var si = 0; si < sets.length; si++) {
                    var mm = /^(.+):(\d+)=(-?[\d.]+)$/.exec(sets[si]);
                    if (!mm) continue;
                    for (var j4 = 1; j4 <= cc.numLayers; j4++) {
                        var d4 = cc.layer(j4).name === mm[1] ? dupli(cc.layer(j4)) : null;
                        if (d4) { d4.property(dx(parseInt(mm[2], 10))).setValue(parseFloat(mm[3])); log("set " + sets[si]); }
                    }
                }
                var tag = src0.id + "_" + modes[m];
                $.setenv("DUPLIX_LOG", (outDir + "/plugin_" + tag + ".log").replace(/\//g, "\\"));
                $.setenv("DUPLIX_CACHE_SALT", "um-" + tag + "-" + new Date().getTime());
                $.setenv("DUPLIX_MERGE_DUMP", (outDir + "/plan_" + tag + ".txt").replace(/\//g, "\\"));
                for (var fi = 0; fi < frames.length; fi++) {
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
                    om.file = new File(outDir + "/r_" + tag + "_" + frames[fi] + "_[#####].tif");
                    rq.timeSpanStart = frames[fi] / src0.frameRate;
                    rq.timeSpanDuration = 1 / src0.frameRate;
                    for (var qi = 1; qi < P.renderQueue.numItems; qi++) {
                        var qq = P.renderQueue.item(qi);
                        if (qq.status === RQItemStatus.QUEUED) qq.render = false;  // done items cannot be changed
                    }
                    P.renderQueue.render();
                    var fl = new Folder(outDir).getFiles("r_" + tag + "_" + frames[fi] + "_*.tif");
                    if (fl.length) fl[0].copy(outDir + "/" + tag + "_" + frames[fi] + ".tif");
                    log("rendered " + tag + " frame " + frames[fi] + (fl.length ? "" : " (no output)"));
                }
            }
        }
        $.setenv("DUPLIX_LOG", "");
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.message + " (line " + e.line + ")");
    }
})();
