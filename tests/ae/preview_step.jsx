// Sent to a running AE (run_preview.ps1): opens the comp named in step_cmd.txt ("<comp name> <frames>
// <interval ms>") in the viewer and, while AE is idle, moves the time one frame every <interval> ms
// (the viewer renders each frame like when scrubbing). Writes step_done.txt at the end.
(function () {
    var outDir = $.getenv("DUPLIX_TEST_DIR");
    // the script arrived (run_preview.ps1 sends it again when this file does not appear; a late first copy and
    // the second one: only the first that arrives runs)
    var got = new File(outDir + "/script_got.txt");
    if (got.exists) return;
    got.open("w"); got.writeln("step"); got.close();
    var f = new File(outDir + "/step_cmd.txt");
    f.encoding = "UTF-8"; f.open("r");
    var parts = f.readln().split(" ");
    f.close();
    var name = parts[0], frames = parseInt(parts[1], 10), interval = parseInt(parts[2], 10);
    var comp = null;
    for (var i = 1; i <= app.project.numItems; i++) {
        var it = app.project.item(i);
        if (it instanceof CompItem && it.name === name) comp = it;
    }
    $.global.__pv = {comp: comp, n: 0, frames: frames, interval: interval, out: outDir};
    $.global.__pvStep = function () {
        var s = $.global.__pv;
        try {
            if (s.n < s.frames) {
                var t = s.n * s.comp.frameDuration;
                s.comp.time = t;
                // AE reuses identical frames; preview_setup.jsx animates the camera so each frame is new.
                // Render the frame (one at a time, so the plugin's span is the frame's latency).
                if ($.getenv("DUPLIX_PREVIEW_PURGE") === "1") app.purge(PurgeTarget.ALL_CACHES);
                var t0 = new Date().getTime();
                s.comp.saveFrameToPng(t, new File(s.out + "/frame_" + s.comp.name + "_" + s.n + ".png"));
                // saveFrameToPng returns before the frame is rendered: frame_ms.txt "<comp> <frame> <start epoch ms>",
                // the PNG's write time minus the start is the frame's latency (render + PNG write)
                var fm = new File(s.out + "/frame_ms.txt");
                fm.open("a"); fm.writeln(s.comp.name + " " + s.n + " " + t0); fm.close();
                s.n++;
                app.scheduleTask("__pvStep()", s.interval, false);
            } else {
                var d = new File(s.out + "/step_done.txt");
                d.open("w"); d.writeln("done " + s.n); d.close();
            }
        } catch (e) {
            var x = new File(s.out + "/step_done.txt");
            x.open("w"); x.writeln("EXCEPTION " + e.message); x.close();
        }
    };
    comp.openInViewer();
    // DUPLIX_PV_ZOOM (e.g. 4): the viewer zoomed in, so that AE draws only a part of the comp
    var zoom = parseFloat($.getenv("DUPLIX_PV_ZOOM") || "");
    if (zoom > 0) {
        try { app.activeViewer.views[0].options.zoom = zoom; } catch (ez) {}
    }
    app.scheduleTask("__pvStep()", 1000, false);
})();
