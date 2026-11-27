// The user's report (11/26 night, Test D): turning the orbit null about Y (40..75 deg) moves where the house cuts
// a copy of the blue flower, while a real layer at the same place (layer 7 of Test D) keeps its cut.
// Opens the COPY named by RANDAMU_USER_AEP, logs the structure, and for each angle of RANDAMU_OC_DEGS renders frame
// RANDAMU_OC_FRAME (default 0) in the variants of RANDAMU_OC_VARIANTS (comma separated):
//   dx    the DupliX layer in normal mode (merge off), the reference layer (RANDAMU_OC_REF, default 7) off
//   dxon  the same with the merge on
//   real  the DupliX layer off, the reference layer on
// Outputs <variant>_<deg>.tif in RANDAMU_TEST_DIR.
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function dupli(L) {
        var p = L.property("ADBE Effect Parade");
        if (!p) return null;
        for (var i = 1; i <= p.numProperties; i++) if (p.property(i).matchName === "RANDAMU Scatter") return p.property(i);
        return null;
    }
    function v3(p) { try { var v = p.value; return v.length ? "[" + v.join(",") + "]" : "" + v; } catch (e) { return "?"; } }
    try {
        app.beginSuppressDialogs();
        app.open(new File($.getenv("RANDAMU_USER_AEP")));
        var P = app.project, main = null;
        for (var i = 1; i <= P.numItems && !main; i++) {
            var it = P.item(i);
            if (!(it instanceof CompItem)) continue;
            for (var j = 1; j <= it.numLayers; j++) if (dupli(it.layer(j))) main = it;
        }
        if (!main) throw new Error("no comp with the effect");
        log("comp '" + main.name + "' " + main.width + "x" + main.height + " renderer=" + main.renderer + " time=" + main.time);
        for (var li = 1; li <= main.numLayers; li++) {
            var L = main.layer(li), T = tg(L), d = dupli(L), dv = "";
            if (d) for (var q = 1; q <= d.numProperties; q++) { try { dv += " " + q + ":" + d.property(q).name + "=" + d.property(q).value; } catch (e0) {} }
            var s = "layer " + li + " '" + L.name + "' 3d=" + L.threeDLayer + " on=" + L.enabled + " parent=" + (L.parent ? L.parent.index : "-");
            if (T) {
                s += " anchor=" + v3(T.property("ADBE Anchor Point")) + " pos=" + v3(T.property("ADBE Position")) +
                    " scale=" + v3(T.property("ADBE Scale"));
                if (L.threeDLayer || L instanceof CameraLayer) s += " orient=" + v3(T.property("ADBE Orientation")) +
                    " rx=" + v3(T.property("ADBE Rotate X")) + " ry=" + v3(T.property("ADBE Rotate Y")) + " rz=" + v3(T.property("ADBE Rotate Z"));
            }
            try { if (L.source) s += " src=" + L.source.name + " " + L.source.width + "x" + L.source.height; } catch (e1) {}
            if (L instanceof CameraLayer) {
                var co = L.property("ADBE Camera Options Group");
                s += " zoom=" + v3(co.property("ADBE Camera Zoom")) + " poi=" + v3(T.property("ADBE Anchor Point")) +
                    " autoOrient=" + L.autoOrient;
            }
            log(s + (d ? " dupli:" + dv : ""));
        }
        var degs = ($.getenv("RANDAMU_OC_DEGS") || "40,50,60,75").split(",");
        var variants = ($.getenv("RANDAMU_OC_VARIANTS") || "dx,real").split(",");
        var refIdx = parseInt($.getenv("RANDAMU_OC_REF") || "7", 10);
        var const_keep = false;
        var frame = parseInt($.getenv("RANDAMU_OC_FRAME") || "0", 10);
        while (P.renderQueue.numItems > 0) P.renderQueue.item(1).remove();
        for (var vi = 0; vi < variants.length; vi++) {
            var vname = variants[vi];
            var c = main.duplicate();
            c.name = "oc_" + vname;
            var cam = null, D = null, R = null;
            for (var a = 1; a <= c.numLayers; a++) {
                if (c.layer(a) instanceof CameraLayer) cam = c.layer(a);
                if (dupli(c.layer(a)) && (!D || c.layer(a).enabled)) D = c.layer(a);
            }
            // RANDAMU_OC_REF=dupe: the reference layer is a copy of the DupliX layer without its effects (twins with
            // the copies' own source)
            if ($.getenv("RANDAMU_OC_REF") === "dupe") {
                R = D.duplicate();
                var fxp = R.property("ADBE Effect Parade");
                while (fxp.numProperties > 0) fxp.property(1).remove();
                R.name = "oc_ref";
            } else {
                R = c.layer(refIdx);
            }
            // RANDAMU_OC_SET ("index=value;..."): DupliX parameters set (e.g. 21=7, the world rotation Y)
            var ocSets = ($.getenv("RANDAMU_OC_SET") || "").split(";");
            for (var os = 0; os < ocSets.length; os++) {
                var om2 = /^(\d+)=(-?[\d.]+)$/.exec(ocSets[os]);
                if (om2) dupli(D).property(parseInt(om2[1], 10)).setValue(parseFloat(om2[2]));
            }
            // RANDAMU_OC_KEEPCAM=1: the orbit null as it is (RANDAMU_OC_DEGS only names the outputs)
            const_keep = $.getenv("RANDAMU_OC_KEEPCAM") === "1";
            if (!const_keep && (!cam || !cam.parent)) throw new Error("camera without an orbit null");
            if (vname === "real") { D.enabled = false; R.enabled = true; }
            else if (vname.indexOf("twin") === 0) {
                // a real layer per copy with the copy's world matrix (RANDAMU_OC_TWINS: "px py pz rx ry rz" per line,
                // copy order), made from the reference layer (same source and anchor) with orientation 0
                var tf = new File($.getenv("RANDAMU_OC_TWINS"));
                tf.open("r");
                var lines = [];
                while (!tf.eof) { var ln = tf.readln(); if (ln) lines.push(ln); }
                tf.close();
                // only these copies' twins (copy numbers, comma separated); RANDAMU_OC_TWIN_ONLY_<variant> for a variant
                // named twin<something>, so that several subsets render in one session
                var only = $.getenv("RANDAMU_OC_TWIN_ONLY_" + vname) || $.getenv("RANDAMU_OC_TWIN_ONLY");
                if (only) {
                    var oa = only.split(","), sub = [];
                    for (var oi = 0; oi < oa.length; oi++) sub.push(lines[parseInt(oa[oi], 10)]);
                    lines = sub;
                }
                D.enabled = false;
                R.enabled = true;
                var RT = tg(R);
                RT.property("ADBE Orientation").setValue([0, 0, 0]);
                RT.property("ADBE Anchor Point").setValue([108.5, 108, 0]);
                RT.property("ADBE Scale").setValue([100, 100, 100]);
                var twinLayers = [];
                for (var ti2 = lines.length - 1; ti2 >= 0; ti2--) {
                    var f6 = lines[ti2].split(" ");
                    var Ln = ti2 === 0 ? R : R.duplicate();
                twinLayers.push(Ln);
                    var LT = tg(Ln);
                    LT.property("ADBE Position").setValue([parseFloat(f6[0]), parseFloat(f6[1]), parseFloat(f6[2])]);
                    LT.property("ADBE Rotate X").setValue(parseFloat(f6[3]));
                    LT.property("ADBE Rotate Y").setValue(parseFloat(f6[4]));
                    LT.property("ADBE Rotate Z").setValue(parseFloat(f6[5]));
                }
                log("twins: " + lines.length + " layers, comp has " + c.numLayers);
                // RANDAMU_OC_PIVOT ("x,y,z") and RANDAMU_OC_PIVOT_RY_<variant> (degrees): every twin parented to a 3D
                // null at the pivot (parenting keeps their place), then the null turned about Y: the twins of one
                // state turned as a whole, without any data of the turned state
                var pv = $.getenv("RANDAMU_OC_PIVOT");
                if (pv) {
                    var pc = pv.split(",");
                    var nl = c.layers.addNull();
                    nl.threeDLayer = true;
                    nl.name = "oc_pivot";
                    tg(nl).property("ADBE Position").setValue([parseFloat(pc[0]), parseFloat(pc[1]), parseFloat(pc[2])]);
                    for (var pi = 0; pi < twinLayers.length; pi++) twinLayers[pi].parent = nl;
                    var pry = parseFloat($.getenv("RANDAMU_OC_PIVOT_RY_" + vname) || "0");
                    tg(nl).property("ADBE Rotate Y").setValue(pry);
                    log("pivot " + pv + " ry " + pry);
                }
            }
            else {
                R.enabled = false;
                D.enabled = true;
                dupli(D).property("RANDAMU Scatter-0027").setValue(vname === "dxon" ? 1 : 0);
            }
            var ry = const_keep ? null : tg(cam.parent).property("ADBE Rotate Y");
            for (var di = 0; di < degs.length; di++) {
                if (ry) {
                    while (ry.numKeys > 0) ry.removeKey(1);
                    ry.setValue(parseFloat(degs[di]));
                }
                var tag = vname + "_" + degs[di];
                $.setenv("RANDAMU_LOG", (outDir + "/plugin_" + tag + ".log").replace(/\//g, "\\"));
                $.setenv("RANDAMU_CACHE_SALT", "oc-" + tag + "-" + new Date().getTime());
                app.purge(PurgeTarget.ALL_CACHES);
                var rq = P.renderQueue.items.add(c);
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
                rq.timeSpanStart = frame * c.frameDuration;
                rq.timeSpanDuration = c.frameDuration;
                for (var qi = 1; qi < P.renderQueue.numItems; qi++) {
                    var qq = P.renderQueue.item(qi);
                    if (qq.status === RQItemStatus.QUEUED) qq.render = false;
                }
                P.renderQueue.render();
                var fl = new Folder(outDir).getFiles("r_" + tag + "_*.tif");
                if (fl.length) fl[0].copy(outDir + "/" + tag + ".tif");
                log("rendered " + tag + (fl.length ? "" : " (no output)") + " cam=" + (cam ? cam.name : "-") + " null=" + (cam && cam.parent ? cam.parent.name : "-"));
            }
        }
        $.setenv("RANDAMU_LOG", "");
        // RANDAMU_OC_SAVE: save the project with the variant comps (oc_<variant>) for a check by eye
        var savePath = $.getenv("RANDAMU_OC_SAVE");
        if (savePath) { P.save(new File(savePath)); log("saved " + savePath); }
        log("done");
    } catch (ex) {
        log("EXCEPTION " + ex.message + " (line " + ex.line + ")");
    }
})();
