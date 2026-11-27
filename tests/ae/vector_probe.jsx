// Vector layers (shape, text) with DupliX, in both render modes. Each case builds a comp with a camera and a
// white target (a 60x60 shape or the text "Ab"), and saves <comp>.png at 0 s:
//   plain_<case>   the target alone, no effect (the reference for one copy)
//   fm1_<case>     Fast (2D), one copy, no ranges: must match plain_<case> (tests/ae/check_vector.py)
//   nm1_<case>     3D Layers, one copy, no ranges: must match plain_<case>
//   fm_<case> / nm_<case>   10 copies scattered 120 px (for the eye; fm and nm should place them alike)
// Cases: shape, text (400x300); shape_odd (401x301); shape_anchor (anchor moved by (17, -11)); shape_half (the
// comp at half resolution); shape_edge (the shape 230 px right of the layer origin, outside the comp; World
// Position X -230 brings the copy back); shape_pre (in a 300x200 precomp, rendered on its own).
// Env: DUPLIX_VP_ONLY (comma list of cases, optional). Writes jsx.log ("done") to $DUPLIX_TEST_DIR.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function dx(id) { return "DupliX-" + ("000" + id).slice(-4); }
    var FAST = dx(37), WPOS = 17;
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
    // mode: "plain" | "fm" | "nm"
    function build(name, c, mode, many) {
        var W = c.W || 400, H = c.H || 300;
        var comp = app.project.items.addComp(name, W, H, 1, 1, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        comp.layers.addCamera("Cam", [W / 2, H / 2]);
        var T = c.kind === "text" ? addText(comp) : addShape(comp, c.size || [60, 60], c.offset || [0, 0]);
        // plain and nm: a 3D layer; fm: a 2D layer (Fast draws the copies on a virtual 3D layer in the initial state)
        var fast = mode === "fm";
        T.threeDLayer = !fast;
        if (c.anchor) tg(T).property("ADBE Anchor Point").setValue(fast ? c.anchor : [c.anchor[0], c.anchor[1], 0]);
        tg(T).property("ADBE Position").setValue(fast ? [W / 2, H / 2] : [W / 2, H / 2, 0]);
        if (mode !== "plain") {
            var e = T.property("ADBE Effect Parade").addProperty("DupliX");
            e.property(FAST).setValue(fast ? 2 : 1);
            e.property(dx(2)).setValue(many ? 10 : 1);
            e.property(dx(3)).setValue(3);
            for (var k = 0; k < 3; k++) e.property(dx(4 + k)).setValue(many ? 120 : 0);
            if (c.wpos && fast) for (var j = 0; j < 3; j++) e.property(dx(WPOS + j)).setValue(c.wpos[j]);
        }
        // the reference for shape_edge: the plain layer moved by the World Position instead
        if (mode === "plain" && c.wpos) tg(T).property("ADBE Position").setValue([W / 2 + c.wpos[0], H / 2 + c.wpos[1], 0]);
        if (c.half) comp.resolutionFactor = [2, 2];
        var wp = "";
        if (mode !== "plain") wp = " world pos " + String(e.property(dx(WPOS)).value) + "," + String(e.property(dx(WPOS + 1)).value);
        var an = tg(T).property("ADBE Anchor Point").value, ps = tg(T).property("ADBE Position").value;
        log(name + ": " + T.matchName + " 3D " + String(T.threeDLayer) + " anchor " + an.join(",") + " pos " +
            ps.join(",") + wp);
        return comp;
    }
    var cases = {
        shape: {kind: "shape"},
        text: {kind: "text"},
        shape_odd: {kind: "shape", W: 401, H: 301},
        shape_anchor: {kind: "shape", anchor: [17, -11]},
        shape_half: {kind: "shape", half: true},
        shape_edge: {kind: "shape", offset: [230, 0], wpos: [-230, 0, 0]},
        shape_pre: {kind: "shape", pre: true}
    };
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var only = ($.getenv("DUPLIX_VP_ONLY") || "").split(",");
        for (var name in cases) {
            if (only[0] && ("," + only.join(",") + ",").indexOf("," + name + ",") < 0) continue;
            var c = cases[name];
            if (c.pre) { c.W = 300; c.H = 200; }
            png(build("plain_" + name, c, "plain"), "plain_" + name);
            png(build("fm1_" + name, c, "fm"), "fm1_" + name);
            png(build("nm1_" + name, c, "nm"), "nm1_" + name);
            png(build("fm_" + name, c, "fm", true), "fm_" + name);
            png(build("nm_" + name, c, "nm", true), "nm_" + name);
            if (c.pre) {
                // the precomp (fm1_shape_pre) inside a 400x300 main comp: the precomp is drawn as is
                var main = app.project.items.addComp("main_shape_pre", 400, 300, 1, 1, 30);
                for (var i = 1; i <= app.project.numItems; i++) {
                    var it = app.project.item(i);
                    if (it.name === "fm1_shape_pre") { main.layers.add(it); break; }
                }
                png(main, "main_shape_pre");
            }
        }
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
