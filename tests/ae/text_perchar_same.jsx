// Does AE draw a 3D text layer the same with per-character 3D on and off (no animators, same transform)? If so, the
// copies of a text layer can be made from the text layer's own per-character machinery (characters x copies as
// sub-layers) and still match real duplicated 3D text layers. No DupliX here. Each case saves off_<case>.png and
// on_<case>.png at 0 s (a comp with a camera, so Classic 3D draws it); tests/ae/check_same.py compares them.
// Env: DUPLIX_TEST_DIR. Writes jsx.log ("done").
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    function png(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " exists=" + f.exists);
    }
    function addText(comp, c) {
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
            td.strokeOverFill = !!c.strokeOver;
        }
        if (c.tracking) td.tracking = c.tracking;
        td.justification = ParagraphJustification.CENTER_JUSTIFY;
        sp.setValue(td);
        return t;
    }
    function build(name, c, perChar) {
        var W = 400, H = 300;
        var comp = app.project.items.addComp(name, W, H, 1, 1, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        var cam = comp.layers.addCamera("Cam", [W / 2, H / 2]);
        if (c.camPos) tg(cam).property("ADBE Position").setValue(c.camPos);
        var T = addText(comp, c);
        T.threeDLayer = true;
        if (perChar) T.threeDPerChar = true;
        tg(T).property("ADBE Position").setValue([W / 2, H / 2 + 20, 0]);
        if (c.rotY) tg(T).property("ADBE Rotate Y").setValue(c.rotY);
        if (c.scale) tg(T).property("ADBE Scale").setValue([c.scale, c.scale, c.scale]);
        if (c.opacity) tg(T).property("ADBE Opacity").setValue(c.opacity);
        log(name + ": perChar " + String(T.threeDPerChar));
        return comp;
    }
    var cases = {
        plain: {},
        rot: {rotY: 35, camPos: [140, 110, -700]},
        scale: {scale: 230},
        tight: {text: "WAVE", tracking: -120, stroke: 6},
        tight_over: {text: "WAVE", tracking: -120, stroke: 6, strokeOver: true},
        tight_rot: {text: "WAVE", tracking: -120, stroke: 6, rotY: 35, camPos: [140, 110, -700]},
        faded: {text: "WAVE", tracking: -120, opacity: 60}
    };
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        for (var name in cases) {
            png(build("off_" + name, cases[name], false), "off_" + name);
            png(build("on_" + name, cases[name], true), "on_" + name);
        }
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
