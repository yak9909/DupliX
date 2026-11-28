// DupliX 2: a project saved with the 409-parameter prototype (before design v4) opened with the v4 build
// (design v4 8.1: it may not keep its settings, but AE must not crash). DUPLIX2_OLD_STEP:
//   save  (run with the prototype's DupliX2.aex) a comp with the effect and some values changed -> old.aep
//   open  (run with the v4 build) opens a copy of old.aep, lists the effect's parameters, renders a frame
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    var step = $.getenv("DUPLIX2_OLD_STEP");
    var keep = $.getenv("DUPLIX2_OLD_AEP");  // where the saved project is kept between the two runs
    try {
        app.beginSuppressDialogs();
        if (step == "save") {
            app.newProject();
            var c = app.project.items.addComp("Old", 400, 300, 1, 1, 30);
            var s = c.layers.addSolid([1, 0, 0], "Red", 40, 40, 1, 1);
            var fx = s.property("ADBE Effect Parade").addProperty("DupliX2");
            log("old effect params=" + fx.numProperties);
            for (var i = 1; i <= Math.min(fx.numProperties, 12); i++) {
                var p = fx.property(i);
                try { p.setValue(p.value + 1); } catch (e) { }
            }
            app.project.save(new File(keep));
            log("saved " + keep);
        } else {
            var src = new File(keep);
            var copy = new File(outDir + "/old_copy.aep");
            src.copy(copy.fsName);  // never the kept file itself (AE saves on close)
            app.open(copy);
            var c = null;
            for (var i = 1; i <= app.project.numItems; i++) if (app.project.item(i).name == "Old") c = app.project.item(i);
            var fx = c.layer(1).property("ADBE Effect Parade").property(1);
            log("opened: effect " + fx.matchName + " params=" + fx.numProperties);
            for (var i = 1; i <= fx.numProperties; i += 10) log(" #" + i + " " + fx.property(i).matchName + " [" + fx.property(i).name + "]");
            var f = new File(outDir + "/old.png");
            c.saveFrameToPng(0, f);
            for (var k = 0; k < 100 && !f.exists; k++) $.sleep(100);
            log("png exists=" + f.exists);
        }
        log("done");
    } catch (e) {
        log("EXCEPTION " + String(e.message) + " (line " + String(e.line) + ")");
    }
})();
