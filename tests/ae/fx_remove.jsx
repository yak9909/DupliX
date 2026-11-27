// Regression test: applying the effect and then removing it (in the same AE session, i.e. the effect is
// kept for undo) must leave the layer exactly as if the effect had never been applied.
// Each case builds "c_<name>" (layer that had the effect) and "r_<name>" (same layer, never had it) and
// saves both PNGs; check_fx_remove.py compares them. Writes jsx.log ("CASE <name>", "done").
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function tg(L) { return L.property("ADBE Transform Group"); }
    var noFx = $.getenv("DUPLIX_NOFX") === "1";  // run without the plugin (control experiment)
    function png(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(0, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        if (!f.exists) throw new Error("png not written: " + name);
    }
    function base(name) {
        var comp = app.project.items.addComp(name, 400, 300, 1, 2, 30);
        comp.layers.addSolid([0.1, 0.1, 0.1], "BG", 400, 300, 1);
        comp.layers.addCamera("Cam", [200, 150]);
        var L = comp.layers.addSolid([1, 1, 1], "Target", 80, 80, 1);
        return {comp: comp, layer: L};
    }
    function addFx(L) {
        var fx = L.property("ADBE Effect Parade").addProperty("DupliX");
        fx.property("DupliX-0002").setValue(5);
        return fx;
    }
    // ops: list of "3d" | "2d" | "fx" | "unfx" | "dup" | "hidedup" | "png" (png the intermediate state)
    function runCase(name, ops) {
        log("CASE " + name);
        var c = base("c_" + name), r = base("r_" + name);
        var dupC = null, dupR = null;
        for (var i = 0; i < ops.length; i++) {
            var op = ops[i];
            if (op === "3d") { c.layer.threeDLayer = true; r.layer.threeDLayer = true; }
            else if (op === "2d") { c.layer.threeDLayer = false; r.layer.threeDLayer = false; }
            else if (op === "fx") { if (!noFx) addFx(c.layer); }
            else if (op === "unfx") {
                if (!noFx) {
                    app.beginUndoGroup("remove fx");
                    c.layer.property("ADBE Effect Parade").property(1).remove();
                    app.endUndoGroup();
                }
            }
            else if (op === "dup") {
                dupC = c.layer.duplicate(); dupR = r.layer.duplicate();
                tg(dupC).property("ADBE Position").setValue([280, 150]);
                tg(dupR).property("ADBE Position").setValue([280, 150]);
            }
            else if (op === "hidedup") { dupC.enabled = false; dupR.enabled = false; }
            else if (op === "png") { png(c.comp, "mid_" + name + "_" + i); }
            else if (op === "undo") { if (!noFx) app.executeCommand(16); }
            else if (op === "fxoff") { if (!noFx) c.layer.property("ADBE Effect Parade").property(1).enabled = false; }
        }
        log("ids c comp=" + c.comp.id + " layer=" + c.layer.id + "  r comp=" + r.comp.id + " layer=" + r.layer.id);
        png(c.comp, "c_" + name);
        log("png c_" + name);
        png(r.comp, "r_" + name);
        log("png r_" + name);
    }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        runCase("3d_remove", ["3d", "fx", "png", "unfx"]);
        runCase("3d_remove_2d", ["3d", "fx", "png", "unfx", "2d"]);
        runCase("2d_remove", ["fx", "png", "unfx"]);
        runCase("2d_to3d_remove", ["fx", "3d", "png", "unfx"]);
        runCase("remove_dup", ["3d", "fx", "png", "unfx", "2d", "dup"]);
        runCase("remove_dup_hide", ["3d", "fx", "png", "unfx", "2d", "dup", "hidedup"]);
        // undo of the removal: the copies must come back (checked by check_fx_remove.py: c != r)
        runCase("remove_undo", ["3d", "fx", "png", "unfx", "png", "undo"]);
        // fx switch off (the effect stays applied): the layer must look as if it had no effect
        runCase("fx_switch_off", ["3d", "fx", "png", "fxoff"]);
        // 3D switch off with the effect still on: a 2D layer is drawn as is
        runCase("fx_then_2d", ["3d", "fx", "png", "2d"]);
        log("done");
    } catch (e) {
        var msg = "?";
        try { msg = String(e.message) + " (line " + String(e.line) + ")"; } catch (e2) { msg = "unprintable"; }
        log("EXCEPTION " + msg);
    }
})();
