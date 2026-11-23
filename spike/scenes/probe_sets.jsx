// THROWAWAY SPIKE — can the hidden "ADBE Layer Sets" feature be reached from script,
// and does AE draw the copies as separate planes?
// Env: RANDAMU_SPIKE_DIR (output dir). Writes jsx.log, probe.aep, f_*.png.
(function () {
    var outDir = $.getenv("RANDAMU_SPIKE_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function dump(p, ind) {
        var s = ind + p.matchName + " [" + p.name + "] type=" + p.propertyType;
        try { if (p.propertyType === PropertyType.PROPERTY) s += " canSet=" + p.canSetValue + " val=" + p.value; } catch (e) { s += " (val err " + e + ")"; }
        try { if (p.propertyType !== PropertyType.PROPERTY) s += " n=" + p.numProperties; } catch (e2) {}
        log(s);
        if (p.propertyType !== PropertyType.PROPERTY) {
            for (var i = 1; i <= p.numProperties; i++) dump(p.property(i), ind + "  ");
        }
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
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var comp = app.project.items.addComp("probe", 400, 300, 1, 1 / 30, 30);
        comp.layers.addSolid([0, 0, 0], "BG", 400, 300, 1);
        var L = comp.layers.addSolid([1, 1, 1], "W", 100, 100, 1);
        L.threeDLayer = true;
        L.property("ADBE Transform Group").property("ADBE Opacity").setValue(50);
        comp.layers.addCamera("Cam", [200, 150]);
        frame(comp, "f_base");

        var sets = L.property("ADBE Layer Sets");
        log("sets=" + (sets ? sets.matchName + " n=" + sets.numProperties : "null"));
        if (!sets) { log("done"); return; }
        log("canAdd(Set)=" + sets.canAddProperty("ADBE Layer Set"));
        log("collapse before=" + L.collapseTransformation);
        var set = null;
        try { set = sets.addProperty("ADBE Layer Set"); log("added set: " + (set ? set.matchName : "null")); }
        catch (e) { log("addProperty EXC " + e); }
        log("collapse after=" + L.collapseTransformation);
        dump(sets, "");
        if (set) {
            var cp = set.property("ADBE Set Copies");
            log("copies prop=" + (cp ? cp.name + " val=" + cp.value : "null"));
            log("before frame f_set5");
            frame(comp, "f_set5");
            log("before setValue(3)");
            if (cp) { try { cp.setValue(3); log("copies set -> " + cp.value); } catch (e3) { log("copies setValue EXC " + e3); } }
            log("before frame f_set3");
            frame(comp, "f_set3");
            var props = set.property("ADBE Set Properties");
            if (props) {
                log("canAdd(SetProperty)=" + props.canAddProperty("ADBE Set Property"));
                try { var sp = props.addProperty("ADBE Set Property"); log("added setprop: " + (sp ? sp.matchName : "null")); }
                catch (e4) { log("add setprop EXC " + e4); }
            }
            dump(sets, "after-prop ");
        }
        var aep = new File(outDir + "/probe.aep");
        app.project.save(aep);
        log("saved");
        app.open(aep);
        var c2 = null;
        for (var i = 1; i <= app.project.numItems; i++) if (app.project.item(i).name === "probe") c2 = app.project.item(i);
        var L2 = c2.layer("W");
        var s2 = L2.property("ADBE Layer Sets");
        log("reopen sets n=" + (s2 ? s2.numProperties : "null"));
        if (s2) dump(s2, "reopen ");
        frame(c2, "f_reopen");
        log("done");
    } catch (e) {
        log("EXCEPTION " + e + " line " + e.line);
    }
})();
