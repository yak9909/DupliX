// DupliX 2: a project saved with P1's build (the graph data version 1, 69 parameters) must open with later builds
// and draw the same (tests/ae/run_dx2_p1_fixture.ps1). DUPLIX_FIXTURE_MODE:
//   make   a 400x300 comp, a 40x40 solid with DupliX 2 as it comes (the 5 x 5 grid), Macro 1 (Count) keyed 2 -> 4
//          over 0..2 s, Macro 2 (Spacing) 60, Random Seed 7; frames 0, 1, 2 s rendered (the effect dumps its
//          elements, DUPLIX2_DUMP), the project saved to DUPLIX_FIXTURE_AEP
//   check  that project opened and the same frames rendered
function dx2(id) { return "DupliX2-" + ("000" + id).slice(-4); }
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    function png(comp, t, name) {
        var f = new File(outDir + "/" + name + ".png");
        comp.saveFrameToPng(t, f);
        for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
        log("png " + name + " t=" + t + " exists=" + f.exists);
    }
    try {
        app.beginSuppressDialogs();
        var mode = $.getenv("DUPLIX_FIXTURE_MODE");
        var aep = new File($.getenv("DUPLIX_FIXTURE_AEP"));
        var comp = null;
        if (mode == "make") {
            app.newProject();
            comp = app.project.items.addComp("P1", 400, 300, 1, 3, 30);
            var s = comp.layers.addSolid([1, 0, 0], "Red", 40, 40, 1, 3);
            var fx = s.property("ADBE Effect Parade").addProperty("DupliX2");
            var m1 = fx.property(dx2(5));
            m1.setValueAtTime(0, 2); m1.setValueAtTime(2, 4);
            fx.property(dx2(6)).setValue(60);
            fx.property(dx2(3)).setValue(7);
            log("made: params " + fx.numProperties);
        } else {
            app.open(aep);
            for (var i = 1; i <= app.project.numItems; i++)
                if (app.project.item(i).name == "P1") comp = app.project.item(i);
            if (!comp) throw new Error("no comp P1 in the project");
            var fx2 = comp.layer(1).property("ADBE Effect Parade").property(1);
            log("opened: effect " + fx2.matchName + " params " + fx2.numProperties + " macro1 keys " + fx2.property(dx2(5)).numKeys +
                " macro2 " + fx2.property(dx2(6)).value + " seed " + fx2.property(dx2(3)).value);
        }
        png(comp, 0, "f0"); png(comp, 1, "f1"); png(comp, 2, "f2");
        if (mode == "make") {
            app.project.save(aep);
            log("saved " + aep.fsName + " exists=" + aep.exists);
        }
        log("done");
    } catch (e) {
        log("EXCEPTION " + String(e.message) + " (line " + String(e.line) + ")");
    }
})();
