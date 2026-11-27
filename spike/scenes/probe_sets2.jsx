// THROWAWAY SPIKE — Layer Sets, part 2: let the plugin call BEE_AVLayer::CreateSet (or set the
// switches) on "Target" from PF_Cmd_SEQUENCE_SETUP, then render.
// Env: DUPLIX_SPIKE_DIR, DUPLIX_PROBE_SETS (create|switch). With "switch" the script adds the
// set group itself first. Writes jsx.log, f_*.png.
(function () {
    var outDir = $.getenv("DUPLIX_SPIKE_DIR");
    var mode = $.getenv("DUPLIX_PROBE_SETS");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function dump(p, ind) {
        var s = ind + p.matchName + " [" + p.name + "]";
        try { if (p.propertyType === PropertyType.PROPERTY) s += " val=" + p.value; } catch (e) { s += " (val err)"; }
        try { if (p.propertyType !== PropertyType.PROPERTY) s += " n=" + p.numProperties; } catch (e2) {}
        log(s);
        if (p.propertyType !== PropertyType.PROPERTY) for (var i = 1; i <= p.numProperties; i++) dump(p.property(i), ind + "  ");
    }
    function frame(comp, name) {
        var f = new File(outDir + "/" + name + ".png");
        try {
            comp.saveFrameToPng(0, f);
            for (var i = 0; i < 100 && !f.exists; i++) $.sleep(100);
            log("frame " + name + " exists=" + f.exists);
        } catch (e) { log("frame " + name + " EXC " + e); }
    }
    try {
        log("mode=" + mode);
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var comp = app.project.items.addComp("probe", 400, 300, 1, 1 / 30, 30);
        comp.layers.addSolid([0, 0, 0], "BG", 400, 300, 1);
        var L;
        if ($.getenv("DUPLIX_TARGET_KIND") === "shape") {
            L = comp.layers.addShape();
            L.name = "Target";
            var grp = L.property("ADBE Root Vectors Group").addProperty("ADBE Vector Group");
            var vecs = grp.property("ADBE Vectors Group");
            vecs.addProperty("ADBE Vector Shape - Rect").property("ADBE Vector Rect Size").setValue([100, 100]);
            vecs.addProperty("ADBE Vector Graphic - Fill").property("ADBE Vector Fill Color").setValue([1, 1, 1]);
            log("target=shape");
        } else if ($.getenv("DUPLIX_TARGET_KIND") === "text") {
            L = comp.layers.addText("AB");
            L.name = "Target";
            var tp = L.property("ADBE Text Properties").property("ADBE Text Document");
            var td = tp.value;
            td.fontSize = 100; td.fillColor = [1, 1, 1]; td.applyFill = true; td.justification = ParagraphJustification.CENTER_JUSTIFY;
            tp.setValue(td);
            L.threeDPerChar = true;
            var an = L.property("ADBE Text Properties").property("ADBE Text Animators").addProperty("ADBE Text Animator");
            an.property("ADBE Text Animator Properties").addProperty("ADBE Text Position 3D").setValue([10, 20, -30]);
            an.property("ADBE Text Animator Properties").addProperty("ADBE Text Rotation").setValue(15);
            log("target=text perChar=" + L.threeDPerChar);
        } else {
            L = comp.layers.addSolid([1, 1, 1], "Target", 100, 100, 1);
        }
        L.threeDLayer = true;
        if ($.getenv("DUPLIX_EFFECT_ON_TARGET") === "1") {
            var fx = L.property("ADBE Effect Parade").addProperty("ADBE Fill");
            fx.property("ADBE Fill-0002").setValue([1, 0, 0, 1]);
            log("effect on target added");
        }
        L.property("ADBE Transform Group").property("ADBE Opacity").setValue(50);
        comp.layers.addCamera("Cam", [200, 150]);
        frame(comp, "f_base");
        if (mode === "switch") {
            L.property("ADBE Layer Sets").addProperty("ADBE Layer Set");
            log("script added set");
        }
        var T = comp.layers.addSolid([0, 0, 0], "T", 10, 10, 1);
        log("before applying effect");
        T.property("ADBE Effect Parade").addProperty("RNDM DupliXSpike");
        log("effect applied");
        T.enabled = false;
        var sets = L.property("ADBE Layer Sets");
        dump(sets, "");
        log("collapse=" + L.collapseTransformation);
        try { app.purge(PurgeTarget.ALL_CACHES); log("purged"); } catch (ep) { log("purge EXC " + ep); }
        frame(comp, "f_after");
        try { app.project.save(new File(outDir + "/probe.aep")); log("saved"); } catch (es) { log("save EXC " + es); }
        try {
            app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES);
            app.open(new File(outDir + "/probe.aep"));
            var c2 = null;
            for (var k = 1; k <= app.project.numItems; k++) if (app.project.item(k).name === "probe") c2 = app.project.item(k);
            var L2 = c2.layer("Target");
            log("reopened: sets n=" + L2.property("ADBE Layer Sets").numProperties + " collapse=" + L2.collapseTransformation);
            frame(c2, "f_reopen");
        } catch (er) { log("reopen EXC " + er); }
        log("done");
    } catch (e) {
        log("EXCEPTION " + e + " line " + e.line);
    }
})();
