// World Transform in normal mode (plan 2026-11-25 Task 3): the copies are placed by copies * G * layer-to-world.
// For each case: wx_<case>.png (layer in its initial 3D state, placement in the World Transform parameters) and
// wl_<case>.png (World Transform at its defaults, the same placement on the layer's own transform).
// check_world_xform.py compares them (max diff <= 1). Writes jsx.log ("CASE <name>", "done").
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var W = 400, H = 300;
    // effect property indices (= RP_*)
    var P_COUNT = 2, P_SEED = 3, P_POS = 4, P_ROT = 7, P_SCMIN = 10, P_SCMAX = 11;
    var P_WPOS = 17, P_WROT = 20, P_WSCALE = 23;
    var img = app.project.importFile(new ImportOptions(new File(outDir + "/../../../spike/assets/probe_img.png")));

    var cases = [
        {name: "rot_xyz", pos: [60, -40, 300], rot: [20, 35, -15], scale: 120},
        {name: "rot_y_only", pos: [0, 0, 0], rot: [0, 50, 0], scale: 100},
        {name: "move_only", pos: [-80, 30, -150], rot: [0, 0, 0], scale: 100}
    ];

    function png(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        if (!f.exists) throw new Error("png not written: " + name);
    }
    function build(c, inParams) {
        var comp = app.project.items.addComp((inParams ? "wx_" : "wl_") + c.name, W, H, 1, 1, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        var cam = comp.layers.addCamera("Cam", [W / 2, H / 2]);
        tg(cam).property("ADBE Position").setValue([120, 60, -900]);
        var L = comp.layers.add(img);
        L.threeDLayer = true;
        L.property("ADBE Transform Group").property("ADBE Scale").setValue([30, 30, 30]);
        var fx = L.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
        fx.property(P_COUNT).setValue(10);
        fx.property(P_SEED).setValue(3);
        fx.property(P_POS).setValue(300);
        fx.property(P_POS + 1).setValue(200);
        fx.property(P_POS + 2).setValue(400);
        fx.property(P_ROT + 2).setValue(90);
        fx.property(P_SCMIN).setValue(60);
        fx.property(P_SCMAX).setValue(130);
        var t = tg(L);
        if (inParams) {
            t.property("ADBE Position").setValue([W / 2, H / 2, 0]);
            for (var k = 0; k < 3; k++) {
                fx.property(P_WPOS + k).setValue(c.pos[k]);
                fx.property(P_WROT + k).setValue(c.rot[k]);
                fx.property(P_WSCALE + k).setValue(c.scale);
            }
        } else {
            // the World Transform position is in layer px, so the layer's 30% scale applies to it
            t.property("ADBE Position").setValue([W / 2 + 0.3 * c.pos[0], H / 2 + 0.3 * c.pos[1], 0.3 * c.pos[2]]);
            t.property("ADBE Rotate X").setValue(c.rot[0]);
            t.property("ADBE Rotate Y").setValue(c.rot[1]);
            t.property("ADBE Rotate Z").setValue(c.rot[2]);
            t.property("ADBE Scale").setValue([30 * c.scale / 100, 30 * c.scale / 100, 30 * c.scale / 100]);
        }
        return comp;
    }
    try {
        for (var i = 0; i < cases.length; i++) {
            var c = cases[i];
            log("CASE " + c.name);
            png(build(c, true), "wx_" + c.name);
            png(build(c, false), "wl_" + c.name);
        }
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.toString() + " line " + e.line);
    }
})();
