// Vector layers (shape, text) with DupliX: how they are drawn today, in both render modes.
// A 400x300 comp with a camera; the target is a white 60x60 shape (or a text "Ab"), 10 copies scattered 120 px.
//   nm_<kind>        3D Layers (the target is a 3D layer)
//   fm_<kind>        Fast (2D) (the target is a 2D layer in its initial state)
//   plain_<kind>     the target alone, no effect (reference for one copy)
//   fm_shape_edge    Fast (2D): the shape sits 230 px right of the layer origin (outside the comp, x = 430);
//                    World Position X -230 brings the copies back: a clipped buffer shows half squares or none
//   fm_shape_big     Fast (2D): a 700x500 shape (larger than the comp), World Scale 40%
// Saves <comp>.png at 0 s. Writes jsx.log ("done") to $DUPLIX_TEST_DIR.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }
    var W = 400, H = 300, FAST = dx(37), WPOS = 17, WSCALE = 23;
    function png(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists);
    }
    function addShape(comp, size, offset) {
        var s = comp.layers.addShape();
        s.name = "Target";
        var g = s.property("ADBE Root Vectors Group").addProperty("ADBE Vector Group");
        var c = g.property("ADBE Vectors Group");
        c.addProperty("ADBE Vector Shape - Rect").property("ADBE Vector Rect Size").setValue(size);
        c.addProperty("ADBE Vector Graphic - Fill").property("ADBE Vector Fill Color").setValue([1, 1, 1, 1]);
        g.property("ADBE Vector Transform Group").property("ADBE Vector Position").setValue(offset);
        return s;
    }
    function addText(comp) {
        var t = comp.layers.addText("Ab");
        t.name = "Target";
        var sp = t.property("ADBE Text Properties").property("ADBE Text Document");
        var td = sp.value;
        td.fontSize = 60;
        td.fillColor = [1, 1, 1];
        td.applyFill = true;
        td.applyStroke = false;
        td.justification = ParagraphJustification.CENTER_JUSTIFY;
        sp.setValue(td);
        return t;
    }
    function build(name, kind, fast, fx, opt) {
        opt = opt || {};
        var comp = app.project.items.addComp(name, W, H, 1, 1, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        comp.layers.addCamera("Cam", [W / 2, H / 2]);
        var T = kind === "text" ? addText(comp) : addShape(comp, opt.size || [60, 60], opt.offset || [0, 0]);
        T.threeDLayer = !fast;
        tg(T).property("ADBE Position").setValue(fast ? [W / 2, H / 2] : [W / 2, H / 2, 0]);
        log(name + ": layer " + T.name + " matchName " + T.matchName + " 3D " + T.threeDLayer +
            " sourceRect " + T.sourceRectAtTime(0, false).toSource());
        if (fx) {
            var e = T.property("ADBE Effect Parade").addProperty("DupliX");
            e.property(FAST).setValue(fast ? 2 : 1);
            e.property(dx(2)).setValue(opt.count || 10);
            e.property(dx(3)).setValue(3);
            for (var k = 0; k < 3; k++) e.property(dx(4 + k)).setValue(opt.range === undefined ? 120 : opt.range);
            if (opt.wpos) for (var j = 0; j < 3; j++) e.property(dx(WPOS + j)).setValue(opt.wpos[j]);
            if (opt.wscale) for (var q = 0; q < 3; q++) e.property(dx(WSCALE + q)).setValue(opt.wscale);
        }
        png(comp, name);
        return comp;
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var kinds = ["shape", "text"];
        for (var i = 0; i < kinds.length; i++) {
            var k = kinds[i];
            build("plain_" + k, k, false, false);
            build("nm_" + k, k, false, true);
            build("nm1_" + k, k, false, true, {count: 1, range: 0});
            build("fm_" + k, k, true, true);
            build("fm1_" + k, k, true, true, {count: 1, range: 0});
        }
        build("plain_shape_edge", "shape", true, false, {offset: [230, 0]});
        build("fm_shape_edge", "shape", true, true, {offset: [230, 0], count: 1, range: 0, wpos: [-230, 0, 0]});
        build("fm_shape_big", "shape", true, true, {size: [700, 500], count: 1, range: 0, wscale: 40});
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
