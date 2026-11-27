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
    // the script arrived (run_preview.ps1 sends it again when this file does not appear; only the first runs)
    var got = new File(outDir + "/script_got.txt");
    if (got.exists) return;
    got.open("w"); got.writeln("setup"); got.close();
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
        // RANDAMU_PV_COUNTS (e.g. "1000,3271") adds more counts. pv_fast<n>: fast mode (RP_FAST = property 15) on a 2D
        // layer, the same copies (the placement is in the World Transform, the layer in its initial state)
        var counts = [1, 20, 100, 380];
        var more = ($.getenv("RANDAMU_PV_COUNTS") || "").split(",");
        for (var mi = 0; mi < more.length; mi++) if (parseInt(more[mi], 10) > 0) counts.push(parseInt(more[mi], 10));
        for (var ci = 0; ci < counts.length; ci++) (function (n) {
            variant("pv_on" + n, function (c, T, f) { f.property(2).setValue(n); f.property(MERGE).setValue(1); });
            variant("pv_off" + n, function (c, T, f) { f.property(2).setValue(n); f.property(MERGE).setValue(0); });
            variant("pv_fast" + n, function (c, T, f) { f.property(2).setValue(n); f.property(15).setValue(1); T.threeDLayer = false; });
        })(counts[ci]);
        variant("pv_none", function (c, T, f) { T.enabled = false; });
        // a large image as the copied layer (RANDAMU_PV_BIG = path of e.g. a 1920x1080 PNG), each count of
        // RANDAMU_PV_COUNTS (else 380): pv_bigS<n> scaled so that it shows as large as the original layer (a large
        // texture, the same pixels on screen), pv_bigL<n> at the layer's scale (more pixels on screen too)
        var bigPath = $.getenv("RANDAMU_PV_BIG") || "";
        if (bigPath) {
            var bigInfo = [];
            var big = P.importFile(new ImportOptions(new File(bigPath)));
            var bigCounts = [];
            for (var bi = 0; bi < more.length; bi++) if (parseInt(more[bi], 10) > 0) bigCounts.push(parseInt(more[bi], 10));
            if (!bigCounts.length) bigCounts.push(380);
            for (var bc = 0; bc < bigCounts.length; bc++) (function (n) {
                function bigVariant(name, shrink) {
                    variant(name, function (c, T, f) {
                        var w0 = T.source.width, h0 = T.source.height;
                        T.replaceSource(big, false);
                        var sp = tg(T).property("ADBE Scale"), s = sp.value, k = shrink ? w0 / big.width : 1;
                        if (sp.numKeys === 0) sp.setValue(s.length > 2 ? [s[0] * k, s[1] * k, s[2] * k] : [s[0] * k, s[1] * k]);
                        // RANDAMU_PV_BIG_MERGE=0: the merge off (AE draws every copy: the reference pixels)
                        f.property(2).setValue(n); f.property(MERGE).setValue($.getenv("RANDAMU_PV_BIG_MERGE") === "0" ? 0 : 1);
                        bigInfo.push(name + ": " + w0 + "x" + h0 + " -> " + big.width + "x" + big.height + " scale x" + k +
                                   " comp " + c.width + "x" + c.height);
                    });
                }
                bigVariant("pv_bigS" + n, true);
                bigVariant("pv_bigL" + n, false);
            })(bigCounts[bc]);
            write("big.txt", bigInfo.join("\n") + "\n");
        }
        // RANDAMU_PV_CROSS=1: copies that cross each other in 3D (small position ranges, rotation ranges 180): AE draws
        // them as clusters (its M buffer). pv_cross<n> merge on, pv_crossoff<n> merge off, for each count of
        // RANDAMU_PV_COUNTS (else 200). Properties 4..6: position ranges, 7..9: rotation ranges
        if ($.getenv("RANDAMU_PV_CROSS") === "1") {
            var crossCounts = [];
            for (var ci2 = 0; ci2 < more.length; ci2++) if (parseInt(more[ci2], 10) > 0) crossCounts.push(parseInt(more[ci2], 10));
            if (!crossCounts.length) crossCounts.push(200);
            for (var cc = 0; cc < crossCounts.length; cc++) (function (n) {
                function cross(c, T, f, merge) {
                    f.property(2).setValue(n);
                    for (var q = 4; q <= 6; q++) f.property(q).setValue(q === 6 ? 40 : 120);
                    for (var r = 7; r <= 9; r++) f.property(r).setValue(180);
                    f.property(MERGE).setValue(merge);
                }
                variant("pv_cross" + n, function (c, T, f) { cross(c, T, f, 1); });
                variant("pv_crossoff" + n, function (c, T, f) { cross(c, T, f, 0); });
            })(crossCounts[cc]);
        }
        // RANDAMU_PV_NEST=1: the copied layer is a precomp with classic 3D layers inside (rotated solids): its texture is
        // drawn by AE's 3D renderer while the outer frame is being drawn (a frame inside a frame). pv_nest<n> for each
        // count of RANDAMU_PV_COUNTS (else 380), merge as RANDAMU_PV_BIG_MERGE (0: off)
        if ($.getenv("RANDAMU_PV_NEST") === "1") {
            var nest = P.items.addComp("nest_src", 300, 200, 1, src.duration, 1 / src.frameDuration);
            var s1 = nest.layers.addSolid([0.9, 0.3, 0.2], "nest_a", 220, 140, 1);
            s1.threeDLayer = true;
            tg(s1).property("ADBE Rotate Y").setValue(35);
            var s2 = nest.layers.addSolid([0.2, 0.5, 0.9], "nest_b", 180, 120, 1);
            s2.threeDLayer = true;
            tg(s2).property("ADBE Rotate X").expression = "time * 60";
            tg(s2).property("ADBE Position").setValue([170, 110, -20]);
            var nestCounts = [];
            for (var ni = 0; ni < more.length; ni++) if (parseInt(more[ni], 10) > 0) nestCounts.push(parseInt(more[ni], 10));
            if (!nestCounts.length) nestCounts.push(380);
            for (var nc = 0; nc < nestCounts.length; nc++) (function (n) {
                variant("pv_nest" + n, function (c, T, f) {
                    T.replaceSource(nest, false);
                    f.property(2).setValue(n); f.property(MERGE).setValue($.getenv("RANDAMU_PV_BIG_MERGE") === "0" ? 0 : 1);
                });
            })(nestCounts[nc]);
        }
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
        // the original comp is open in a viewer and hidden AE draws it too (with its saved settings): its DupliX
        // layers off so that its draws do not mix into the variants' timing (a copy of the user's file)
        for (var oi = 1; oi <= src.numLayers; oi++) if (fxOf(src.layer(oi))) src.layer(oi).enabled = false;
        write("variants.txt", lines.join("\n") + "\n");
    } catch (e) {
        write("variants.txt", "EXCEPTION " + e.message + " (line " + e.line + ")\n");
    }
})();
