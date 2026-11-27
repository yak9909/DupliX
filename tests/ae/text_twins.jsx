// Copies of a text layer in 3D Layers against real duplicated text layers ("twins"). For each case:
//   dx_<case>    the text layer with DupliX (3D Layers, N copies, ranges), saved at 0 s. The plugin appends every
//                copy's M4 (row vectors, the copy's matrix in the layer's space) to $DUPLIX_DBG_COPIES while it draws.
//   twin_<case>  the same comp without the effect and with one real text layer per copy, placed by that M4
//                (position = translation + the layer's position, scale = row lengths, rotation Z from row 0; the
//                cases only use rotation about Z).
// tests/ae/check_twins.py compares them (RGB). Env: DUPLIX_TEST_DIR, DUPLIX_DBG_COPIES (set before AE starts).
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }
    function png(comp, name, time) {
        var f = new File(outDir + "/" + name + ".png");
        app.purge(PurgeTarget.ALL_CACHES);  // a hidden layer the copies show is not a dependency AE knows of
        comp.saveFrameToPng(time || 0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists);
        return f.exists;
    }
    var W = 400, H = 300, POS = [W / 2, H / 2, 0];
    // the other layer the odd copies show (DUPLIX_DBG_OTHER=2: created before the target, so it is layer 2)
    function addOther(comp, c, on) {
        if (c.otherAnim) {  // a precomp whose content moves, the layer started 0.5 s early (its own time runs ahead)
            if (!c.src) {
                c.src = app.project.items.addComp("OtherSrc", c.other[0], c.other[1], 1, 4, 30);
                var mv = c.src.layers.addSolid([0.9, 0.25, 0.1], "Mover", 20, c.other[1], 1);
                tg(mv).property("ADBE Position").setValueAtTime(0, [10, c.other[1] / 2]);
                tg(mv).property("ADBE Position").setValueAtTime(2, [c.other[0] - 10, c.other[1] / 2]);
            }
            var pl = comp.layers.add(c.src);
            pl.startTime = -0.5;
            pl.threeDLayer = true;
            tg(pl).property("ADBE Anchor Point").setValue([0, 0, 0]);
            tg(pl).property("ADBE Position").setValue(POS);
            pl.enabled = on;
            return pl;
        }
        var b = comp.layers.addSolid([0.9, 0.25, 0.1], "Other", c.other[0], c.other[1], 1);
        b.threeDLayer = true;
        tg(b).property("ADBE Anchor Point").setValue([0, 0, 0]);
        tg(b).property("ADBE Position").setValue(POS);
        b.enabled = on;
        return b;
    }
    function addText(comp, c) {
        if (c.solid) {  // the control: a solid (its copies are known to match AE)
            var so = comp.layers.addSolid([1, 1, 1], "Target", 60, 40, 1);
            so.threeDLayer = true;
            tg(so).property("ADBE Anchor Point").setValue([0, 0, 0]);
            tg(so).property("ADBE Position").setValue(POS);
            return so;
        }
        if (c.shape) {  // the other continuously rasterized layer (already supported)
            var s = comp.layers.addShape();
            s.name = "Target";
            var g = s.property("ADBE Root Vectors Group").addProperty("ADBE Vector Group");
            var gc = g.property("ADBE Vectors Group");
            gc.addProperty("ADBE Vector Shape - Ellipse").property("ADBE Vector Ellipse Size").setValue([60, 40]);
            gc.addProperty("ADBE Vector Graphic - Fill").property("ADBE Vector Fill Color").setValue([1, 1, 1, 1]);
            s.threeDLayer = true;
            tg(s).property("ADBE Position").setValue(POS);
            return s;
        }
        var t = comp.layers.addText(c.text || "Ab");
        t.name = "Target";
        var sp = t.property("ADBE Text Properties").property("ADBE Text Document");
        var td = sp.value;
        td.fontSize = c.size || 60;
        td.fillColor = [1, 1, 1];
        td.applyFill = true;
        td.applyStroke = !!c.stroke;
        if (c.stroke) {
            td.strokeColor = [0.9, 0.2, 0.1];
            td.strokeWidth = c.stroke;
        }
        if (c.tracking) td.tracking = c.tracking;
        td.justification = ParagraphJustification.CENTER_JUSTIFY;
        sp.setValue(td);
        t.threeDLayer = true;
        tg(t).property("ADBE Position").setValue(POS);
        return t;
    }
    function base(name) {
        var comp = app.project.items.addComp(name, W, H, 1, 4, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        comp.layers.addCamera("Cam", [W / 2, H / 2]);
        return comp;
    }
    function readCopies(compId) {
        var f = new File($.getenv("DUPLIX_DBG_COPIES"));
        var rows = [];
        if (!f.exists) return rows;
        f.open("r");
        while (!f.eof) {
            var ln = f.readln();
            if (!ln) continue;
            var a = ln.split(" ");
            if (parseInt(a[0], 10) !== compId) continue;
            var m = [];
            for (var q = 0; q < 16; q++) m.push(parseFloat(a[3 + q]));
            rows[parseInt(a[2], 10)] = m;
        }
        f.close();
        return rows;
    }
    var cases = {
        pos: {n: 10, pos: [240, 160, 400]},
        scale: {n: 10, pos: [240, 160, 400], scale: [40, 260]},
        rotz: {n: 10, pos: [240, 160, 300], rotz: 180, scale: [60, 180]},
        stroke: {n: 8, text: "WAVE", tracking: -120, stroke: 6, pos: [200, 150, 300], scale: [50, 200], rotz: 90},
        big: {n: 6, size: 120, pos: [160, 120, 600], scale: [100, 300]},
        solid_scale: {n: 10, solid: true, pos: [240, 160, 400], scale: [40, 260]},
        solid_rotz: {n: 10, solid: true, pos: [240, 160, 300], rotz: 180, scale: [60, 180]},
        other_time: {n: 10, solid: true, other: [100, 24], otherAnim: true, time: 1, pos: [240, 160, 400], rotz: 180, scale: [60, 180]},
        other_same: {n: 10, solid: true, other: [60, 40], pos: [240, 160, 400], rotz: 180, scale: [60, 180]},
        other_size: {n: 10, solid: true, other: [100, 24], pos: [240, 160, 400], rotz: 180, scale: [60, 180]},
        solid_fill: {n: 10, solid: true, fill: true, pos: [240, 160, 400], rotz: 180, scale: [60, 180]},
        text_fill: {n: 8, fill: true, text: "WAVE", pos: [240, 160, 300], rotz: 90, scale: [60, 180]},
        shape_pos: {n: 10, shape: true, pos: [240, 160, 400]},
        shape_scale: {n: 10, shape: true, pos: [240, 160, 400], scale: [40, 260]}
    };
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var only = ($.getenv("DUPLIX_TT_ONLY") || "").split(",");
        for (var name in cases) {
            if (only[0] && ("," + only.join(",") + ",").indexOf("," + name + ",") < 0) continue;
            var c = cases[name];
            var d = base("dx_" + name);
            if (c.other) addOther(d, c, false);  // eye off: seen only through the copies
            var T = addText(d, c);
            var e = T.property("ADBE Effect Parade").addProperty("DupliX");
            e.property(dx(37)).setValue(1);  // 3D Layers
            e.property(dx(27)).setValue($.getenv("DUPLIX_TT_MERGE") === "1" ? 1 : 0);  // Optimize
            e.property(dx(2)).setValue(c.n);
            e.property(dx(3)).setValue(7);
            for (var k = 0; k < 3; k++) e.property(dx(4 + k)).setValue(c.pos[k]);
            for (var r = 0; r < 3; r++) e.property(dx(7 + r)).setValue(r === 2 && c.rotz ? c.rotz : 0);
            e.property(dx(10)).setValue(c.scale ? c.scale[0] : 100);
            e.property(dx(11)).setValue(c.scale ? c.scale[1] : 100);
            png(d, "dx_" + name, c.time);
            var rows = readCopies(d.id);
            log(name + ": comp id " + d.id + " copies read " + rows.length);
            var tw = base("twin_" + name);
            for (var i = rows.length - 1; i >= 0; i--) {
                var m = rows[i];
                if (!m) continue;
                var L = c.other && (i & 1) ? addOther(tw, c, true) : addText(tw, c);
                L.name = "twin " + i;
                var sx = Math.sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
                var sy = Math.sqrt(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
                var sz = Math.sqrt(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
                tg(L).property("ADBE Scale").setValue([sx * 100, sy * 100, sz * 100]);
                tg(L).property("ADBE Rotate Z").setValue(Math.atan2(m[1], m[0]) * 180 / Math.PI);
                tg(L).property("ADBE Position").setValue([POS[0] + m[12], POS[1] + m[13], POS[2] + m[14]]);
                if (c.fill) {  // the copy's color (DUPLIX_DBG_TINT on the DupliX side): AE's Fill on the real layer
                    var PAL = [[1, 0, 0], [0, 1, 0], [0, 0, 1], [1, 1, 0], [0, 1, 1], [1, 0, 1]];
                    var fe = L.property("ADBE Effect Parade").addProperty("ADBE Fill");
                    fe.property("ADBE Fill-0002").setValue(PAL[i % 6]);
                }
                log("twin " + i + " pos " + (POS[0] + m[12]).toFixed(3) + "," + (POS[1] + m[13]).toFixed(3) + "," +
                    (POS[2] + m[14]).toFixed(3) + " scale " + (sx * 100).toFixed(3) + " rz " +
                    (Math.atan2(m[1], m[0]) * 180 / Math.PI).toFixed(3) + " m2 " + m[2] + " m6 " + m[6]);
            }
            png(tw, "twin_" + name, c.time);
        }
        log("done");
    } catch (ex) {
        var msg = "?";
        try { msg = String(ex.message) + " (line " + String(ex.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
