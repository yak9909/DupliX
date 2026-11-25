// Fast mode switched on by script (plan 2026-11-25 Task 5): the layer's transform goes into the World Transform
// and the layer to its initial 2D state; switching off makes it 3D again. For each case saves
//   <case>_before.png (normal mode), <case>_after.png (fast mode on), and per case
//   toggle_plain_back.png (fast off again), toggle_parent_undo.png (after one Undo).
// Logs "STATE <case>_<tag> key=value ..." lines for check_transfer.py. Env: RANDAMU_TEST_DIR, RANDAMU_FAST_IMAGE.
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var W = 400, H = 300, FAST = 15, WPOS = 17, WROT = 20, WSCALE = 23;
    var img = app.project.importFile(new ImportOptions(new File($.getenv("RANDAMU_FAST_IMAGE"))));

    function png(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(comp.time, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        if (!f.exists) throw new Error("png not written: " + name);
    }
    // one value at a time (concatenating several property values in one expression failed, dev log 11/25)
    function state(tag, L, fx) {
        var t = tg(L), parts = [];
        function g(k, f) { var v; try { v = String(f()); } catch (e) { v = "ERR"; } parts.push(k + "=" + v.replace(/ /g, "")); }
        g("3d", function () { return L.threeDLayer; });
        g("parent", function () { return L.parent ? "yes" : "none"; });
        g("posKeys", function () { return t.property("ADBE Position").numKeys; });
        g("pos", function () { return t.property("ADBE Position").value; });
        g("rotY", function () { return t.property("ADBE Rotate Y").value; });
        g("scale", function () { return t.property("ADBE Scale").value; });
        g("wposX", function () { return fx.property(WPOS).value; });
        g("wrotY", function () { return fx.property(WROT + 1).value; });
        g("fast", function () { return fx.property(FAST).value; });
        log("STATE " + tag + " " + parts.join(" "));
    }
    function scene(name) {
        var comp = app.project.items.addComp(name, W, H, 1, 2, 30);
        comp.layers.addSolid([0.1, 0.1, 0.12], "BG", W, H, 1);
        var cam = comp.layers.addCamera("Cam", [W / 2, H / 2]);
        tg(cam).property("ADBE Position").setValue([150, 100, -800]);
        var L = comp.layers.add(img);
        L.threeDLayer = true;
        var fx = L.property("ADBE Effect Parade").addProperty("RANDAMU Scatter");
        fx.property(2).setValue(10);
        fx.property(3).setValue(3);
        fx.property(4).setValue(140); fx.property(5).setValue(90); fx.property(6).setValue(250);
        fx.property(9).setValue(90);
        fx.property(10).setValue(60); fx.property(11).setValue(130);
        return {comp: comp, layer: L, fx: fx};
    }
    try {
        app.beginSuppressDialogs();
        // a layer with position, orientation, rotations, uniform scale; the World Transform already set
        log("CASE toggle_plain");
        var a = scene("toggle_plain");
        var ta = tg(a.layer);
        ta.property("ADBE Position").setValue([230, 130, 80]);
        ta.property("ADBE Orientation").setValue([0, 15, 0]);
        ta.property("ADBE Rotate X").setValue(10);
        ta.property("ADBE Rotate Y").setValue(20);
        ta.property("ADBE Rotate Z").setValue(30);
        ta.property("ADBE Scale").setValue([110, 110, 110]);
        a.fx.property(WPOS).setValue(20);
        a.fx.property(WROT + 2).setValue(15);
        for (var k = 0; k < 3; k++) a.fx.property(WSCALE + k).setValue(90);
        png(a.comp, "toggle_plain_before");
        a.fx.property(FAST).setValue(1);
        state("toggle_plain_after", a.layer, a.fx);
        png(a.comp, "toggle_plain_after");
        a.fx.property(FAST).setValue(0);
        state("toggle_plain_back", a.layer, a.fx);
        png(a.comp, "toggle_plain_back");

        // the user's scene shape: a child of an orbiting null (Y 26 deg), with a Z position
        log("CASE toggle_parent");
        var b = scene("toggle_parent");
        var nul = b.comp.layers.addNull();
        nul.threeDLayer = true;
        tg(nul).property("ADBE Position").setValue([200, 150, 0]);
        tg(nul).property("ADBE Rotate Y").setValue(26);
        b.layer.parent = nul;  // AE keeps the world orientation by changing the child's: reset it to follow the null
        tg(b.layer).property("ADBE Orientation").setValue([0, 0, 0]);
        tg(b.layer).property("ADBE Position").setValue([60, -20, 120]);
        png(b.comp, "toggle_parent_before");
        b.fx.property(FAST).setValue(1);
        state("toggle_parent_after", b.layer, b.fx);
        png(b.comp, "toggle_parent_after");
        app.executeCommand(16);  // Edit > Undo: everything back in one step
        state("toggle_parent_undo", b.layer, b.fx);
        png(b.comp, "toggle_parent_undo");

        // keyframed position: the value at the current frame is transferred, the keyframes are removed
        log("CASE toggle_anim");
        var c = scene("toggle_anim");
        var pc = tg(c.layer).property("ADBE Position");
        pc.setValueAtTime(0, [150, 150, 0]);
        pc.setValueAtTime(1, [250, 150, 200]);
        c.comp.time = 0.5;
        png(c.comp, "toggle_anim_before");
        c.fx.property(FAST).setValue(1);
        state("toggle_anim_after", c.layer, c.fx);
        png(c.comp, "toggle_anim_after");
        // separated position dimensions: refused before anything changes (final review, item 1)
        log("CASE toggle_separated");
        var e = scene("toggle_separated");
        var pe = tg(e.layer).property("ADBE Position");
        pe.dimensionsSeparated = true;
        tg(e.layer).property("ADBE Position_0").setValue(230);
        tg(e.layer).property("ADBE Rotate Y").setValue(20);
        e.fx.property(FAST).setValue(1);
        state("toggle_separated_after", e.layer, e.fx);

        // a keyframed World Transform with a layer not in its initial state: refused (final review, item 2)
        log("CASE toggle_world_keys");
        var f = scene("toggle_world_keys");
        tg(f.layer).property("ADBE Rotate Y").setValue(26);
        f.fx.property(WPOS).setValueAtTime(0, -40);
        f.fx.property(WPOS).setValueAtTime(1, 40);
        f.fx.property(FAST).setValue(1);
        state("toggle_world_keys_after", f.layer, f.fx);
        log("STATE toggle_world_keys_keys n=" + f.fx.property(WPOS).numKeys + " k2=" + f.fx.property(WPOS).keyValue(2));

        // a keyframed World Transform with the layer already in its initial 3D state: nothing to move
        log("CASE toggle_initial_keys");
        var g2 = scene("toggle_initial_keys");
        tg(g2.layer).property("ADBE Position").setValue([W / 2, H / 2, 0]);
        g2.fx.property(WPOS).setValueAtTime(0, -40);
        g2.fx.property(WPOS).setValueAtTime(1, 40);
        g2.comp.time = 0.5;
        png(g2.comp, "toggle_initial_keys_before");
        g2.fx.property(FAST).setValue(1);
        state("toggle_initial_keys_after", g2.layer, g2.fx);
        log("STATE toggle_initial_keys_keys n=" + g2.fx.property(WPOS).numKeys + " k2=" + g2.fx.property(WPOS).keyValue(2));
        png(g2.comp, "toggle_initial_keys_after");
        log("done");
    } catch (e) {
        log("EXCEPTION " + e.toString() + " line " + e.line);
    }
})();
