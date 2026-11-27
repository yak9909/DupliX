// Writes $DUPLIX_TEST_DIR/ping.txt with the time and the open project's name (checks that a script
// sent with `AfterFX.exe -r` reaches an already running AE).
(function () {
    var f = new File($.getenv("DUPLIX_TEST_DIR") + "/ping.txt");
    f.encoding = "UTF-8";
    f.open("w");
    f.writeln(new Date().toString() + " project=" + (app.project.file ? app.project.file.name : "(none)"));
    f.close();
})();
