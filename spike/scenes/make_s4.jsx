// THROWAWAY SPIKE — S4: does the effect re-render when another layer changes, inside ONE AE session?
// Opens spike/logs/s1/s1.aep (built by run_s1.ps1), then for each case:
//   render "<case>_a" (comp + its _ref) -> change the target (in both) -> render "<case>_b".
// A stale cache shows up as "<case>_b" not matching "<case>_b_ref" (check_s4.py), and as
// 0 SmartRender calls after the "== step <case>_b" marker in plugin.log.
// Env: DUPLIX_SPIKE_DIR (output dir), DUPLIX_SPIKE_LOG (plugin log), DUPLIX_S4_STEPS ("0" = only step 0).
(function () {
    var outDir = $.getenv("DUPLIX_SPIKE_DIR");
    var logFile = new File(outDir + "/s4_jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    var plog = new File($.getenv("DUPLIX_SPIKE_LOG"));
    function mark(s) { plog.open("a"); plog.writeln("== step " + s); plog.close(); log("== step " + s); }
    var onlyStep0 = $.getenv("DUPLIX_S4_STEPS") === "0";

    try {
        app.beginSuppressDialogs();
        app.open(new File(outDir + "/s1.aep"));
        var rq = app.project.renderQueue;
        while (rq.numItems > 0) rq.item(1).remove();

        function comp(name) {
            for (var i = 1; i <= app.project.numItems; i++) {
                var it = app.project.item(i);
                if (it instanceof CompItem && it.name === name) return it;
            }
            throw new Error("comp not found: " + name);
        }
        function addItem(c, tag) {
            var item = rq.items.add(c);
            var om = item.outputModule(1);
            om.applyTemplate("RNDM_TIFF_A");
            om.file = new File(outDir + "/" + tag + "_[#####].tif");
            return item;
        }
        // Render <compName> and its _ref as "<tag>" / "<tag>_ref".
        function renderPair(compName, tag) {
            mark(tag);
            addItem(comp(compName), tag);
            addItem(comp(compName + "_ref"), tag + "_ref");
            var t0 = new Date().getTime();
            rq.render();
            log(tag + " rendered in " + (new Date().getTime() - t0) + "ms");
            while (rq.numItems > 0) rq.item(1).remove();
        }
        // Apply fn(comp) to the comp and to its _ref duplicate.
        function both(compName, fn) { fn(comp(compName)); fn(comp(compName + "_ref")); }
        function tr(l) { return l.property("ADBE Transform Group"); }

        // S5b mode 1: the plugin takes geometry from AEGP. Remove the corner expressions so a
        // pass cannot come from expression-driven param changes.
        if ($.getenv("DUPLIX_S5B") === "1") {
            var stripped = 0;
            for (var ii = 1; ii <= app.project.numItems; ii++) {
                var ci = app.project.item(ii);
                if (!(ci instanceof CompItem)) continue;
                for (var li = 1; li <= ci.numLayers; li++) {
                    var fxp = ci.layer(li).property("ADBE Effect Parade");
                    if (!fxp) continue;
                    for (var fi = 1; fi <= fxp.numProperties; fi++) {
                        var fx = fxp.property(fi);
                        if (fx.matchName !== "RNDM DupliXSpike") continue;
                        for (var pi = 1; pi <= fx.numProperties; pi++) {
                            var pp = fx.property(pi);
                            if (pp.canSetExpression && pp.expression !== "") { pp.expression = ""; stripped++; }
                        }
                        // product-like: no layer slots at all (AE then cannot track Target through them)
                        if ($.getenv("DUPLIX_S4_NOSLOT") === "1") {
                            fx.property("Slot 1").setValue(0);
                            fx.property("Slot 2").setValue(0);
                        }
                    }
                }
            }
            log("S5b: stripped " + stripped + " expressions, noslot=" + $.getenv("DUPLIX_S4_NOSLOT"));
        }

        // Step 0: same comp twice, nothing changed. The second render should come from the cache.
        renderPair("v0_identity", "s0_a");
        renderPair("v0_identity", "s0_b");

        if (!onlyStep0) {
            // name, comp, change, kind ("guid" = only the AEGP receipt GUID can notice it,
            // "expr" = the corner expressions change our params anyway)
            var cases = [
                ["mask", "t_mask", "guid", function (c) {
                    var s = new Shape(); s.vertices = [[0, 0], [400, 0], [400, 300]]; s.closed = true;
                    c.layer("Target").property("ADBE Mask Parade").property(1).property("ADBE Mask Shape").setValue(s);
                }],
                ["effect", "t_effect", "guid", function (c) {
                    c.layer("Target").property("ADBE Effect Parade").property(1)
                        .property("ADBE Geometry2-0002").setValue([100, 150]);
                }],
                ["precomp", "t_precomp", "guid", null],   // shared precomp: changed once below
                ["matte_mask", "t_matte", "guid", function (c) {
                    var m = c.layer("Matte").property("ADBE Mask Parade").addProperty("ADBE Mask Atom");
                    var s = new Shape(); s.vertices = [[0, 0], [250, 0], [0, 250]]; s.closed = true;
                    m.property("ADBE Mask Shape").setValue(s);
                }],
                ["opacity0", "v1_moved", "guid", function (c) {
                    tr(c.layer("Target")).property("ADBE Opacity").setValue(0);
                }],
                ["move", "v2_anchor", "expr", function (c) {
                    tr(c.layer("Target")).property("ADBE Position").setValue([1100, 650, 0]);
                }],
                ["camera", "v3_targetZ", "expr", function (c) {
                    tr(c.layer("Cam")).property("ADBE Position").setValue([1160, 540, -2666.7]);
                }]
            ];
            // S7 (every layer is a target): layer list changes
            if ($.getenv("DUPLIX_S7") === "1") {
                function addSolid3d(c, name, col, w, h, pos) {
                    var l = c.layers.addSolid(col, name, w, h, 1);
                    l.threeDLayer = true;
                    tr(l).property("ADBE Position").setValue(pos);
                    c.layer("Src").moveToBeginning();   // the effect layer stays on top
                    return l;
                }
                cases.push(["hide", "v0_identity", "guid", function (c) { c.layer("Target").enabled = false; }]);
                cases.push(["add", "t_png", "guid", function (c) {
                    addSolid3d(c, "Extra", [0.3, 0.8, 0.3], 250, 250, [600, 350, 0]);
                }]);
                cases.push(["delete", "t_png", "guid", function (c) { c.layer("Extra").remove(); }]);
                cases.push(["reorder", "t_shape", "guid", function (c) {
                    c.layer("Target").moveToBeginning();
                    c.layer("Src").moveToBeginning();
                }]);
                cases.push(["many", "t_text", "guid", function (c) {
                    for (var gy = 0; gy < 6; gy++)
                        for (var gx = 0; gx < 8; gx++)
                            addSolid3d(c, "G" + gy + "_" + gx, [0.5, 0.5, 0.9], 60, 60,
                                       [450 + gx * 140, 180 + gy * 130, (gx + gy) * 40]);
                }]);
            }
            for (var k = 0; k < cases.length; k++) {
                var cs = cases[k];
                log("case " + cs[0] + " comp=" + cs[1] + " kind=" + cs[2]);
                renderPair(cs[1], cs[0] + "_a");
                if (cs[3]) both(cs[1], cs[3]);
                else {
                    var pc = comp("PC_plain");
                    tr(pc.layer("A")).property("ADBE Position").setValue([260, 90]);
                }
                renderPair(cs[1], cs[0] + "_b");
            }
        }
        log("S4 done");
    } catch (e) {
        log("EXCEPTION: " + e.toString() + " line " + e.line);
    }
    try { app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES); } catch (e2) {}
    app.quit();
})();
