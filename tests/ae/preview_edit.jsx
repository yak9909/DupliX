// Sent to a running AE (run_preview.ps1 -Edit): checks that an edit of DupliX's parameters at the frame being
// shown is drawn with the new values (the hooks cache the parameters per layer and time). For the comps named
// in step_cmd.txt ("<comp>[,<comp>...] <frame>"), it draws the frame in the viewer, then for each edit (count,
// seed, opacity range) changes the value with no time change, draws the same frame again, and also draws a fresh
// duplicate of the comp (nothing cached for it) at that frame. edit_<comp>_<n>.png must equal ref_<comp>_<n>.png
// (check_preview_edit.py). Writes step_done.txt at the end.
(function () {
    var outDir = $.getenv("RANDAMU_TEST_DIR");
    var f = new File(outDir + "/step_cmd.txt");
    f.encoding = "UTF-8"; f.open("r");
    var parts = f.readln().split(" ");
    f.close();
    var names = parts[0].split(","), frame = parseInt(parts[1], 10);
    function fxOf(L) {
        var p = L.property("ADBE Effect Parade");
        if (!p) return null;
        for (var i = 1; i <= p.numProperties; i++) if (p.property(i).matchName === "RANDAMU Scatter") return p.property(i);
        return null;
    }
    function compNamed(n) {
        for (var i = 1; i <= app.project.numItems; i++) {
            var it = app.project.item(i);
            if (it instanceof CompItem && it.name === n) return it;
        }
        return null;
    }
    function dupliOf(c) {
        var lname = $.getenv("RANDAMU_PV_LAYER") || "";
        var T = null;
        for (var a = 1; a <= c.numLayers; a++) if (fxOf(c.layer(a)) && (!lname || c.layer(a).name === lname)) T = c.layer(a);
        return fxOf(T);
    }
    // the edits, applied one after another (property index = RP_* in src/effect/randamu_params.h)
    var edits = [
        function (fx) { fx.property(2).setValue(fx.property(2).value + 7); },          // count
        function (fx) { fx.property(3).setValue(fx.property(3).value + 1); },          // seed
        function (fx) { fx.property(12).setValue(20); fx.property(13).setValue(60); }, // opacity range
        function (fx) { fx.property(2).setValue(fx.property(2).value - 3); },          // count back down
        function (fx) {                                                                // another effect on the layer
            var L = fx.parentProperty.parentProperty, P = L.property("ADBE Effect Parade");
            for (var i = 1; i <= P.numProperties; i++) {
                var e = P.property(i);
                if (e.matchName !== "ADBE Fill") continue;
                for (var q = 1; q <= e.numProperties; q++)
                    if (e.property(q).propertyValueType === PropertyValueType.COLOR) e.property(q).expression = "[0.9, 0.2, 0.1, 1]";
                return;
            }
            fx.property(3).setValue(fx.property(3).value + 1);  // no Fill: the seed again
        }
    ];
    // a queue of steps; each waits for the previous PNG (saveFrameToPng returns before the frame is written)
    var steps = [], log = [];
    for (var ci = 0; ci < names.length; ci++) (function (name) {
        steps.push({png: outDir + "/edit_" + name + "_0.png", run: function (s) {
            var c = compNamed(name);
            c.openInViewer();
            c.time = frame * c.frameDuration;
            c.saveFrameToPng(c.time, new File(s.png));
        }});
        for (var e = 0; e < edits.length; e++) (function (k) {
            steps.push({png: outDir + "/edit_" + name + "_" + (k + 1) + ".png", run: function (s) {
                var c = compNamed(name);
                edits[k](dupliOf(c));
                c.saveFrameToPng(frame * c.frameDuration, new File(s.png));
            }});
            steps.push({png: outDir + "/ref_" + name + "_" + (k + 1) + ".png", run: function (s) {
                // nothing cached: AE's caches purged, and the hooks' caches are per comp and layer
                app.purge(PurgeTarget.ALL_CACHES);
                var d = compNamed(name).duplicate();
                d.name = name + "_ref" + (k + 1);
                d.saveFrameToPng(frame * d.frameDuration, new File(s.png));
            }});
        })(e);
    })(names[ci]);
    $.global.__pe = {steps: steps, i: 0, started: false, t0: 0, out: outDir, log: log};
    $.global.__peStep = function () {
        var s = $.global.__pe;
        try {
            if (s.started) {
                var cur = s.steps[s.i];
                if (!new File(cur.png).exists) {
                    if (new Date().getTime() - s.t0 > 60000) throw new Error("timeout waiting for " + cur.png);
                    app.scheduleTask("__peStep()", 200, false);
                    return;
                }
                s.i++;
                s.started = false;
            }
            if (s.i < s.steps.length) {
                s.started = true;
                s.t0 = new Date().getTime();
                s.steps[s.i].run(s.steps[s.i]);
                app.scheduleTask("__peStep()", 300, false);
            } else {
                var d = new File(s.out + "/step_done.txt");
                d.open("w"); d.writeln("done " + s.i); d.close();
            }
        } catch (e) {
            var x = new File(s.out + "/step_done.txt");
            x.open("w"); x.writeln("EXCEPTION " + e.message + " (step " + s.i + ")"); x.close();
        }
    };
    app.scheduleTask("__peStep()", 1000, false);
})();
