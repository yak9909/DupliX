// Sent to a running AE (run_preview.ps1) whose open project is a copy of a user scene: builds variant
// comps from the comp holding the randamu layer and writes "<variant> <comp id>" lines to variants.txt.
//   pv_asis   : as is
//   pv_one    : randamu with 1 copy and all ranges 0 (looks like no effect; measurable by the plugin)
//   pv_real10 : 10 real duplicates, each with randamu (1 copy, ranges 0), original hidden
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    function write(name, text) { var f = new File(outDir + "/" + name); f.encoding = "UTF-8"; f.open("w"); f.write(text); f.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function fxOf(L) {
        var p = L.property("ADBE Effect Parade");
        if (!p) return null;
        for (var i = 1; i <= p.numProperties; i++) if (p.property(i).matchName === "RANDAMU Scatter") return p.property(i);
        return null;
    }
    function plain1(f) { f.property(2).setValue(1); for (var q = 4; q <= 9; q++) f.property(q).setValue(0); }
    var lines = [];
    try {
        app.beginSuppressDialogs();
        var src = null, P = app.project;
        for (var i = 1; i <= P.numItems && !src; i++) {
            var it = P.item(i);
            if (!(it instanceof CompItem)) continue;
            for (var j = 1; j <= it.numLayers; j++) if (fxOf(it.layer(j))) { src = it; break; }
        }
        if (!src) throw new Error("no comp with randamu");
        var names = [];
        for (var k = 1; k <= src.numLayers; k++) if (fxOf(src.layer(k))) names.push(src.layer(k).name + " enabled=" + src.layer(k).enabled + " count=" + fxOf(src.layer(k)).property(2).value);
        write("layers.txt", src.name + "\n" + names.join("\n") + "\n");
        function variant(name, fn) {
            var c = src.duplicate();
            c.name = name;
            var T = null;
            for (var a = 1; a <= c.numLayers; a++) if (fxOf(c.layer(a))) T = c.layer(a);
            var lname = $.getenv("RANDAMU_PV_LAYER") || "";
            if (lname) for (var a2 = 1; a2 <= c.numLayers; a2++) if (c.layer(a2).name === lname && fxOf(c.layer(a2))) T = c.layer(a2);
            fn(c, T, fxOf(T));
            // every frame different (AE reuses identical frames), footage caches kept: move the camera a bit
            for (var b = 1; b <= c.numLayers; b++) {
                var Lb = c.layer(b);
                if (Lb instanceof CameraLayer) tg(Lb).property("ADBE Position").expression = "value + [time * 40, 0, 0]";
            }
            lines.push(name + " " + c.id + " " + c.frameDuration);
        }
        variant("pv_asis", function () {});
        // sampling cost probes: lower layer quality / bilinear sampling (classic 3D samples every pixel)
        variant("pv_draft", function (c, T, f) { T.quality = LayerQuality.DRAFT; });
        variant("pv_bilinear", function (c, T, f) { T.samplingQuality = LayerSamplingQuality.BILINEAR; });
        variant("pv_one", function (c, T, f) { plain1(f); });
        // normal mode with and without merging (the merge checkbox: disk ID RD_MERGE = 27)
        var MERGE = "RANDAMU Scatter-0027";
        variant("pv_norm10", function (c, T, f) { f.property(MERGE).setValue(0); });
        variant("pv_merge10", function (c, T, f) { f.property(MERGE).setValue(1); });
        variant("pv_norm100", function (c, T, f) { f.property(2).setValue(100); f.property(MERGE).setValue(0); });
        variant("pv_merge100", function (c, T, f) { f.property(2).setValue(100); f.property(MERGE).setValue(1); });
        // draft quality (nearest sampling): the group plane is pixel-aligned, so its pixels should not change
        variant("pv_merge10_draft", function (c, T, f) { f.property(MERGE).setValue(1); T.quality = LayerQuality.DRAFT; });
        variant("pv_merge100_draft", function (c, T, f) { f.property(2).setValue(100); f.property(MERGE).setValue(1); T.quality = LayerQuality.DRAFT; });
        variant("pv_norm10_draft", function (c, T, f) { f.property(MERGE).setValue(0); T.quality = LayerQuality.DRAFT; });
        variant("pv_real10", function (c, T, f) {
            plain1(f);
            for (var d = 0; d < 10; d++) {
                var D = T.duplicate();
                var p = tg(D).property("ADBE Position").value;
                tg(D).property("ADBE Position").setValue([p[0] + (d - 5) * 150, p[1] + ((d * 53) % 400) - 200, p[2] + ((d * 71) % 400) - 200]);
            }
            T.enabled = false;
        });
        // merge on / off with a copy count on the layer named RANDAMU_PV_LAYER (else the last DupliX layer):
        // pv_on20, pv_off100, ..., pv_none (that layer off: the scene without its copies)
        var counts = [20, 100, 380];
        for (var ci = 0; ci < counts.length; ci++) (function (n) {
            variant("pv_on" + n, function (c, T, f) { f.property(2).setValue(n); f.property(MERGE).setValue(1); });
            variant("pv_off" + n, function (c, T, f) { f.property(2).setValue(n); f.property(MERGE).setValue(0); });
        })(counts[ci]);
        variant("pv_none", function (c, T, f) { T.enabled = false; });
        // the layer's pixels change every frame (a Fill before DupliX whose color follows the time): a texture kept
        // from another frame would show
        function animFill(T) {
            T.property("ADBE Effect Parade").addProperty("ADBE Fill").moveTo(1);
            var fill = T.property("ADBE Effect Parade").property(1);  // moveTo invalidates the reference
            for (var q = 1; q <= fill.numProperties; q++) {
                var pr = fill.property(q);
                if (pr.propertyValueType === PropertyValueType.COLOR) {
                    pr.expression = "[0.5 + 0.5 * Math.sin(time * 9), 0.5 + 0.5 * Math.cos(time * 7), 0.4, 1]";
                    break;
                }
            }
        }
        variant("pv_anim_on100", function (c, T, f) { f.property(2).setValue(100); f.property(MERGE).setValue(1); animFill(T); });
        variant("pv_anim_off100", function (c, T, f) { f.property(2).setValue(100); f.property(MERGE).setValue(0); animFill(T); });
        write("variants.txt", lines.join("\n") + "\n");
    } catch (e) {
        write("variants.txt", "EXCEPTION " + e.message + " (line " + e.line + ")\n");
    }
})();
