// THROWAWAY SPIKE — how far is the cut-out method (A') from the correct picture?
// No plugin involved: two comps built from stock AE features.
//   Correct : layer X (occluder) is ABOVE the copy -> AE composites X over the copy.
//   Approx  : the copy is on TOP, its alpha cut by X's alpha (track matte, inverted alpha),
//             X is below it over the background. That is exactly what A' produces.
// Columns: X at 100% / 75% / 50% / 25% opacity, feathered edge, Add, Multiply.
// Env: RANDAMU_SPIKE_DIR (output dir).
(function () {
    var outDir = $.getenv("RANDAMU_SPIKE_DIR");
    var logFile = new File(outDir + "/jsx.log");
    logFile.encoding = "UTF-8";
    function log(s) { logFile.open("a"); logFile.writeln(s); logFile.close(); }
    try {
        app.beginSuppressDialogs();
        app.newProject();
        app.project.bitsPerChannel = 8;
        var W = 1820, H = 400, COLW = 260, CY = 230;
        var cols = [
            {label: "100%", op: 100, col: [1, 1, 1]},
            {label: "75%", op: 75, col: [1, 1, 1]},
            {label: "50%", op: 50, col: [1, 1, 1]},
            {label: "25%", op: 25, col: [1, 1, 1]},
            {label: "feather", op: 100, col: [1, 1, 1], feather: true},
            {label: "Add", op: 100, col: [1, 0.45, 0.1], mode: BlendingMode.ADD},
            {label: "Multiply", op: 100, col: [0.2, 0.75, 1], mode: BlendingMode.MULTIPLY}
        ];
        function tr(l) { return l.property("ADBE Transform Group"); }
        function makeCopy(comp, x, y) {
            var l = comp.layers.addShape();
            l.name = "Copy";
            var root = l.property("ADBE Root Vectors Group");
            function item(kind, setup, col) {
                var g = root.addProperty("ADBE Vector Group");
                var c = g.property("ADBE Vectors Group");
                var s = c.addProperty(kind);
                setup(s);
                c.addProperty("ADBE Vector Graphic - Fill").property("ADBE Vector Fill Color").setValue(col);
            }
            // first item is drawn on top
            item("ADBE Vector Shape - Rect", function (s) { s.property("ADBE Vector Rect Size").setValue([200, 24]); }, [1, 1, 1]);
            item("ADBE Vector Shape - Ellipse", function (s) { s.property("ADBE Vector Ellipse Size").setValue([110, 110]); }, [0.1, 0.15, 0.4]);
            item("ADBE Vector Shape - Rect", function (s) {
                s.property("ADBE Vector Rect Size").setValue([200, 200]);
                s.property("ADBE Vector Rect Roundness").setValue(30);
            }, [1, 0.8, 0.2]);
            tr(l).property("ADBE Position").setValue([x, y]);
            return l;
        }
        function makeX(comp, c, x, y) {
            var l = comp.layers.addSolid(c.col, "X", 160, 300, 1);
            tr(l).property("ADBE Position").setValue([x + 50, y]);
            tr(l).property("ADBE Opacity").setValue(c.op);
            if (c.feather) {
                var m = l.property("ADBE Mask Parade").addProperty("ADBE Mask Atom");
                var s = new Shape();
                s.vertices = [[20, 20], [140, 20], [140, 280], [20, 280]];
                s.closed = true;
                m.property("ADBE Mask Shape").setValue(s);
                m.property("ADBE Mask Feather").setValue([70, 70]);
            }
            if (c.mode) l.blendingMode = c.mode;
            return l;
        }
        function build(name, approx) {
            var comp = app.project.items.addComp(name, W, H, 1, 1 / 30, 30);
            var bg = comp.layers.addSolid([0.25, 0.3, 0.38], "BG", W, H, 1);
            var cb = bg.property("ADBE Effect Parade").addProperty("ADBE Checkerboard");
            for (var i = 0; i < cols.length; i++) {
                var c = cols[i];
                var x = COLW * i + COLW / 2 - 25;
                if (!approx) {
                    makeCopy(comp, x, CY);
                    makeX(comp, c, x, CY);           // X above the copy
                } else {
                    makeX(comp, c, x, CY);           // X over the background
                    var cp = makeCopy(comp, x, CY);  // copy on top...
                    var matte = makeX(comp, c, x, CY);
                    matte.name = "Xmatte";
                    matte.blendingMode = BlendingMode.NORMAL;
                    cp.moveToBeginning();
                    cp.setTrackMatte(matte, TrackMatteType.ALPHA_INVERTED);   // ...with X's alpha cut out
                    matte.enabled = false;
                }
                var t = comp.layers.addText(c.label);
                var td = t.property("ADBE Text Properties").property("ADBE Text Document");
                var doc = td.value; doc.fontSize = 34; doc.applyFill = true; doc.fillColor = [1, 1, 1];
                doc.justification = ParagraphJustification.CENTER_JUSTIFY;
                td.setValue(doc);
                tr(t).property("ADBE Position").setValue([x + 25, 50]);
            }
            var rq = app.project.renderQueue.items.add(comp);
            var om = rq.outputModule(1);
            try { om.applyTemplate("RNDM_TIFF_A"); } catch (e) { log("template err " + e); }
            om.file = new File(outDir + "/" + name + "_[#####].tif");
            log(name + " built");
        }
        build("correct", false);
        build("approx", true);
        var aep = new File(outDir + "/approx.aep");
        app.project.save(aep);
        log("saved " + aep.fsName);
    } catch (e) {
        log("EXCEPTION: " + e.toString() + " line " + e.line);
    }
    app.project.close(CloseOptions.DO_NOT_SAVE_CHANGES);
    app.quit();
})();
