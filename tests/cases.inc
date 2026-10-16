// Test cases for agssetup. This file is appended to a copy of agssetup.cpp by
// tests/run.sh (see there for why), so everything in agssetup.cpp - including
// private members, thanks to -fno-access-control - is directly reachable.
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <cstdio>

namespace {

int g_fails = 0, g_checks = 0, g_skips = 0;

void check(bool ok, const QString &what) {
    ++g_checks;
    if (!ok)
        ++g_fails;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    std::fflush(stdout);
}

void skip(const QString &what) {
    ++g_skips;
    std::printf("SKIP %s\n", qPrintable(what));
}

// Answers any message box that appears, by its title; remembers which appeared.
class Clicker : public QObject {
public:
    QMap<QString, QMessageBox::StandardButton> answers;
    QMessageBox::StandardButton fallback = QMessageBox::Yes;
    QStringList seen;

    Clicker() {
        timer.setInterval(25);
        connect(&timer, &QTimer::timeout, this, [this]() {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box)
                return;
            seen << box->windowTitle();
            QAbstractButton *b = box->button(answers.value(box->windowTitle(), fallback));
            if (b)
                b->click();
            else
                box->accept();
        });
        timer.start();
    }

private:
    QTimer timer;
};

QString writeFile(const QString &path, const QByteArray &data) {
    QDir().mkpath(QFileInfo(path).path());
    QFile f(path);
    f.open(QIODevice::WriteOnly);
    f.write(data);
    return path;
}

// Makes a folder AGS Setup accepts as a game.
QString makeGame(const QString &path) {
    writeFile(path + "/acsetup.cfg", "[misc]\ntitle=Test\n");
    return QFileInfo(path).absoluteFilePath();
}

QListWidgetItem *itemFor(AGSSetup &w, const QString &path) {
    for (int i = 0; i < w.libraryList->count(); ++i)
        if (w.libraryList->item(i)->data(Qt::UserRole).toString() == path)
            return w.libraryList->item(i);
    return nullptr;
}

// A minimal slang shader with one tweakable value: output = source * BRIGHT.
const char *kBrightShader = R"(#version 450
#pragma parameter BRIGHT "Brightness" 1.0 0.0 2.0 0.1
layout(push_constant) uniform Push {
    vec4 SourceSize; vec4 OriginalSize; vec4 OutputSize; uint FrameCount; float BRIGHT;
} params;
layout(std140, set = 0, binding = 0) uniform UBO { mat4 MVP; } global;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 vTexCoord;
void main() { gl_Position = global.MVP * Position; vTexCoord = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;
layout(set = 0, binding = 2) uniform sampler2D Source;
void main() { FragColor = vec4(texture(Source, vTexCoord).rgb * params.BRIGHT, 1.0); }
)";

QString makeBrightPreset(const QString &dir) {
    writeFile(dir + "/bright.slang", kBrightShader);
    return writeFile(dir + "/bright.slangp",
                     "shaders = 1\nshader0 = bright.slang\nfilter_linear0 = false\n");
}

// Tiny HTTP server serving one file (or an error) to whatever asks.
class FileServer : public QObject {
public:
    QByteArray body;
    int status = 200;
    int requests = 0;
    QTcpServer server;

    FileServer() {
        server.listen(QHostAddress::LocalHost, 0);
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                connect(s, &QTcpSocket::readyRead, s, [this, s]() {
                    if (!s->peek(4096).contains("\r\n\r\n"))
                        return;
                    s->readAll();
                    ++requests;
                    const QByteArray head = QByteArray("HTTP/1.1 ") + QByteArray::number(status) +
                        (status == 200 ? " OK" : " Not Found") + "\r\nContent-Type: application/gzip\r\n"
                        "Content-Length: " + QByteArray::number(status == 200 ? body.size() : 0) +
                        "\r\nConnection: close\r\n\r\n";
                    s->write(head);
                    if (status == 200)
                        s->write(body);
                    s->disconnectFromHost();
                });
            }
        });
    }
    QString url() const { return "http://127.0.0.1:" + QString::number(server.serverPort()) + "/shaders.tar.gz"; }
};

QByteArray makeTarball(const QString &workDir, const QString &topDir, const QMap<QString, QByteArray> &files) {
    const QString root = workDir + "/tarsrc-" + topDir;
    QDir(root).removeRecursively();
    for (auto it = files.constBegin(); it != files.constEnd(); ++it)
        writeFile(root + "/" + topDir + "/" + it.key(), it.value());
    const QString out = workDir + "/" + topDir + ".tar.gz";
    QProcess tar;
    tar.start("tar", {"-czf", out, "-C", root, topDir});
    tar.waitForFinished(20000);
    QFile f(out);
    f.open(QIODevice::ReadOnly);
    return f.readAll();
}

// ---------------------------------------------------------------------------

void testLibraryRemoveMissing(AGSSetup &w, const QString &work) {
    std::puts("== library: a missing game can still be removed");
    Clicker clicker;
    const QString gone = work + "/lib-gone";
    writeFile(QDir(configDir()).filePath("lib-gone-1.cfg"), ("[librabridge]\ngame_path=" + gone + "\n").toUtf8());
    w.refreshLibrary();
    QListWidgetItem *item = itemFor(w, gone);
    check(item && item->text().contains("missing"), "missing entry is listed as missing");
    w.libraryList->setCurrentItem(item);
    w.onLibraryItemChosen(item);
    QTest::qWait(60);
    check(w.libraryList->currentItem() == item, "entry stays selected after the failed open");
    check(w.libraryRemoveBtn->isEnabled(), "Remove is enabled for it");
    w.onGamePathEdited();
    check(w.libraryList->currentItem() == item && w.libraryRemoveBtn->isEnabled(), "path box losing focus keeps it");
    w.libraryRemoveBtn->click();
    check(!QFileInfo::exists(QDir(configDir()).filePath("lib-gone-1.cfg")), "its settings file is deleted");
    check(!itemFor(w, gone), "it is gone from the list");
}

void testLocate(AGSSetup &w, const QString &work) {
    std::puts("== locate: a moved game keeps its settings");
    Clicker clicker;
    const QString oldPath = makeGame(work + "/old/Moved");
    check(w.selectGame(oldPath), "game opens");
    const QString oldCfg = w.cfgPath;
    IniFile ini;
    ini.load(oldCfg);
    ini.setValue("zz", "marker", "42");
    ini.save(oldCfg);
    writeFile(w.logPath, "last launch log");
    const QString oldLog = w.logPath;

    QDir().mkpath(work + "/new");
    QDir().rename(oldPath, work + "/new/Moved");
    const QString newPath = QFileInfo(work + "/new/Moved").absoluteFilePath();
    w.refreshLibrary();
    QListWidgetItem *item = itemFor(w, oldPath);
    check(item && item->text().contains("missing"), "it shows as missing after the move");

    // not a game: refused, nothing changes
    clicker.seen.clear();
    w.relocateTo(item, work + "/new");
    check(clicker.seen.contains("Locate game") && QFileInfo::exists(oldCfg), "a folder that is no game is refused");

    w.relocateTo(item, newPath);
    check(!QFileInfo::exists(oldCfg), "old settings file is gone");
    check(w.currentGame == newPath, "the game is open at its new place");
    IniFile moved;
    moved.load(w.cfgPath);
    check(moved.value("zz", "marker") == "42", "its settings came along");
    check(moved.value("librabridge", "game_path") == newPath, "game_path points to the new place");
    check(QFileInfo::exists(w.cfgPath) && w.cfgPath != oldCfg, "settings live under the new name");
    QFile log(w.logPath);
    check(log.open(QIODevice::ReadOnly) && log.readAll() == "last launch log" && !QFileInfo::exists(oldLog),
          "the last-launch log moved too");
    int entries = 0;
    for (int i = 0; i < w.libraryList->count(); ++i)
        if (w.libraryList->item(i)->data(Qt::UserRole).toString() == newPath || w.libraryList->item(i)->data(Qt::UserRole).toString() == oldPath)
            ++entries;
    check(entries == 1 && !itemFor(w, oldPath), "exactly one library entry, at the new path");

    // the new place is already in the library: ask, and honour "No"
    std::puts("-- locate onto a game that already has settings");
    const QString a = makeGame(work + "/coll/A");
    const QString b = makeGame(work + "/coll/B");
    w.selectGame(b);
    IniFile bi;
    bi.load(w.cfgPath);
    bi.setValue("zz", "marker", "from-B");
    bi.save(w.cfgPath);
    w.selectGame(a);
    IniFile ai;
    ai.load(w.cfgPath);
    ai.setValue("zz", "marker", "from-A");
    ai.save(w.cfgPath);
    const QString aCfg = w.cfgPath;
    QDir().rename(a, work + "/coll/A-gone");
    w.refreshLibrary();
    item = itemFor(w, a);
    clicker.answers["Locate game"] = QMessageBox::No;
    w.relocateTo(item, b);
    check(QFileInfo::exists(aCfg), "answering No changes nothing");
    clicker.answers["Locate game"] = QMessageBox::Yes;
    item = itemFor(w, a);
    w.relocateTo(item, b);
    check(!QFileInfo::exists(aCfg) && w.currentGame == b, "answering Yes moves it onto B");
    IniFile over;
    over.load(w.cfgPath);
    check(over.value("zz", "marker") == "from-A", "B now has A's settings");
}

void testLocateOpenGameWithEdits(AGSSetup &w, const QString &work) {
    std::puts("== locate: the open game has unsaved edits");
    Clicker clicker;
    const QString oldPath = makeGame(work + "/dirty/Edited");
    w.selectGame(oldPath);
    const QString oldCfg = w.cfgPath;
    w.shaderParams = "X=1";
    check(w.isDirty(), "the game has unsaved edits");
    QDir().mkpath(work + "/dirty-new");
    QDir().rename(oldPath, work + "/dirty-new/Edited");
    const QString newPath = QFileInfo(work + "/dirty-new/Edited").absoluteFilePath();
    w.refreshLibrary();

    clicker.answers["Unsaved changes"] = QMessageBox::Cancel;
    w.relocateTo(itemFor(w, oldPath), newPath);
    check(QFileInfo::exists(oldCfg) && w.currentGame == oldPath, "Cancel leaves everything as it was");

    clicker.answers["Unsaved changes"] = QMessageBox::Save;
    w.relocateTo(itemFor(w, oldPath), newPath);
    check(!QFileInfo::exists(oldCfg), "after Save the old settings file does not come back");
    check(w.currentGame == newPath && w.shaderParams == "X=1" && !w.isDirty(), "the saved edit moved with the game");
    int entries = 0;
    for (int i = 0; i < w.libraryList->count(); ++i) {
        const QString path = w.libraryList->item(i)->data(Qt::UserRole).toString();
        entries += (path == oldPath || path == newPath);
    }
    check(entries == 1 && !itemFor(w, oldPath), "one library entry, no ghost at the old path");
}

void testLibraryCap(const QString &work) {
    std::puts("== library: the cap keeps the first games by name");
    for (const char *n : {"Capalpha", "Capbravo", "Capcharlie"})
        writeFile(QDir(configDir()).filePath(QString("cap-%1.cfg").arg(n)),
                  (QString("[librabridge]\ngame_path=%1/cap/%2\n").arg(work, n)).toUtf8());
    const QVector<GameEntry> all = configuredGames(1000);
    const QVector<GameEntry> cut = configuredGames(all.size() - 1);
    check(cut.size() == all.size() - 1 && all.size() > 3, "one is cut off");
    check(!cut.isEmpty() && cut.last().path == all[all.size() - 2].path,
          "what is cut is the last by name, not an arbitrary one");
}

void testParams(AGSSetup &w, const QString &work) {
    std::puts("== shader parameters: storage and hand-over to the engine");
    check(FormatParams(ParseParams("A=1;B=0.5")) == "A=1;B=0.5", "format/parse round trip");
    check(ParseParams(" A = 2 ; ;=3;B;C=x;D=1;E;").size() == 2, "junk entries are dropped, tidy ones kept");
    check(ParseParams("A=1.5").value("A") == 1.5, "decimal point");
    check(FormatParams(ParseParams("A=2.4000000954")) == "A=2.4000001", "9 significant digits");
    check(ParseParams("A=nan;B=inf;C=-inf;D=1").size() == 1, "non-finite values are dropped");

    const QString game = makeGame(work + "/params/Game");
    check(w.selectGame(game), "game opens");
    w.setShaderPreset("/shaders/a.slangp");
    w.shaderParams = "BRIGHT=0.5";
    check(w.isDirty(), "changing parameters makes the game dirty");
    check(w.save(), "save");
    IniFile saved;
    saved.load(w.cfgPath);
    check(saved.value("librabridge", "shader_params") == "BRIGHT=0.5", "parameters are written under [librabridge]");

    const QString other = makeGame(work + "/params/Other");
    w.selectGame(other);
    check(w.shaderParams.isEmpty(), "another game starts without them");
    w.selectGame(game);
    check(w.shaderParams == "BRIGHT=0.5" && !w.isDirty(), "they come back when the game is reopened");

    w.shaderEnabled->setChecked(true);
    w.driver->setCurrentIndex(w.driver->findData("OGL"));
    QProcessEnvironment env = w.engineEnvironment(true);
    check(env.value("AGS_LIBRASHADER_PARAMS") == "BRIGHT=0.5", "engine gets AGS_LIBRASHADER_PARAMS");
    check(!w.engineEnvironment(false).contains("AGS_LIBRASHADER_PARAMS"), "...but not when the shader is off");

    w.setShaderPreset("/shaders/b.slangp");
    check(w.shaderParams.isEmpty(), "another preset drops the parameters of the old one");
    check(!w.engineEnvironment(true).contains("AGS_LIBRASHADER_PARAMS"), "and none is handed to the engine");
    w.save();
    IniFile cleared;
    cleared.load(w.cfgPath);
    check(!cleared.contains("librabridge", "shader_params"), "the key is removed from the file");
}

void testListParams(const QString &work, bool haveLibra) {
    std::puts("== shader parameters: what librashader reports");
    if (!haveLibra) {
        skip("no librashader.so available (set LIBRASHADER_SO)");
        return;
    }
    const QString preset = makeBrightPreset(work + "/shader");
    QString err;
    const QVector<ShaderParam> list = ListPresetParams(preset, &err);
    check(list.size() == 1 && list[0].name == "BRIGHT", "finds the #pragma parameter (" + err + ")");
    if (list.size() == 1) {
        check(qAbs(list[0].initial - 1.0) < 1e-4 && list[0].minimum == 0 && qAbs(list[0].maximum - 2.0) < 1e-4,
              "default, minimum and maximum are read");
        check(list[0].description == "Brightness", "description is read");
    }
    err.clear();
    ListPresetParams(work + "/shader/missing.slangp", &err);
    check(!err.isEmpty(), "a missing preset reports an error");
}

QColor sample(const QImage &img, int x, int y) { return img.pixelColor(x, y); }

bool near(const QColor &c, int r, int g, int b, int tol = 4) {
    return qAbs(c.red() - r) <= tol && qAbs(c.green() - g) <= tol && qAbs(c.blue() - b) <= tol;
}

QString colorText(const QColor &c) { return QString("(%1,%2,%3)").arg(c.red()).arg(c.green()).arg(c.blue()); }

void testPreview(const QString &work, bool haveLibra) {
    std::puts("== preview: picture and live parameters (real OpenGL)");
    if (QGuiApplication::platformName() != "xcb") {
        skip("needs a real display (run under Xvfb): platform is " + QGuiApplication::platformName());
        return;
    }
    // top-left red, top-right green, bottom-left blue, bottom-right white; 64x48
    QImage pic(64, 48, QImage::Format_RGB32);
    for (int y = 0; y < 48; ++y)
        for (int x = 0; x < 64; ++x)
            pic.setPixelColor(x, y, y < 24 ? (x < 32 ? QColor(255, 0, 0) : QColor(0, 255, 0))
                                           : (x < 32 ? QColor(0, 0, 255) : QColor(255, 255, 255)));
    const QString picPath = work + "/pic.png";
    pic.save(picPath);
    QImage flat(64, 48, QImage::Format_RGB32);
    flat.fill(QColor(120, 80, 40));
    flat.save(work + "/flat.png");

    ShaderPreview p;
    p.show();
    QTest::qWait(300);
    // 4:3 picture in a 640x360 widget: 480x360 starting at x=80
    p.setSourceImage(picPath);
    p.refresh(QString());
    QTest::qWait(300);
    QImage shot = p.grabFramebuffer();
    check(shot.width() >= 640, "preview renders (" + QString::number(shot.width()) + "x" + QString::number(shot.height()) + ")");
    check(near(sample(shot, 200, 90), 255, 0, 0) && near(sample(shot, 440, 90), 0, 255, 0) &&
          near(sample(shot, 200, 270), 0, 0, 255) && near(sample(shot, 440, 270), 255, 255, 255),
          "picture is the right way up with the right colours " + colorText(sample(shot, 200, 90)) +
          colorText(sample(shot, 440, 90)) + colorText(sample(shot, 200, 270)) + colorText(sample(shot, 440, 270)));
    check(near(sample(shot, 20, 180), 26, 26, 31, 8), "the picture keeps its aspect ratio (bars at the sides)");

    p.setSourceImage(work + "/not-an-image.png");
    p.refresh(QString());
    QTest::qWait(300);
    p.grabFramebuffer();
    check(p.statusText().contains("Could not read the image"), "an unreadable picture says so");
    // the same name, now holding a real picture: Refresh must pick it up
    flat.save(work + "/not-an-image.png");
    p.refresh(QString());
    QTest::qWait(300);
    shot = p.grabFramebuffer();
    check(!p.statusText().contains("Could not read") && near(sample(shot, 320, 180), 120, 80, 40),
          "a picture fixed under the same name is re-read on refresh");

    // transparency in a picture must not show through to the window behind
    QImage clear(64, 48, QImage::Format_ARGB32);
    clear.fill(QColor(255, 0, 0, 0));
    clear.save(work + "/clear.png");
    p.setSourceImage(work + "/clear.png");
    p.refresh(QString());
    QTest::qWait(300);
    shot = p.grabFramebuffer();
    check(sample(shot, 320, 180).alpha() == 255, "a transparent picture is shown opaque");

    if (!haveLibra) {
        skip("no librashader.so: the filter chain part of the preview");
        return;
    }
    const QString preset = makeBrightPreset(work + "/shader");
    p.setSourceImage(work + "/flat.png");
    p.setParameters({});
    p.refresh(preset);
    QTest::qWait(400);
    shot = p.grabFramebuffer();
    check(p.statusText().contains("Loaded"), "preset loads: " + p.statusText());
    check(near(sample(shot, 320, 180), 120, 80, 40), "defaults pass the picture through " + colorText(sample(shot, 320, 180)));

    p.setParameterValue("BRIGHT", 0.5);
    QTest::qWait(200);
    shot = p.grabFramebuffer();
    check(near(sample(shot, 320, 180), 60, 40, 20), "BRIGHT=0.5 halves it, live " + colorText(sample(shot, 320, 180)));
    p.setParameterValue("BRIGHT", 0.0);
    QTest::qWait(200);
    shot = p.grabFramebuffer();
    check(near(sample(shot, 320, 180), 0, 0, 0), "BRIGHT=0 blacks it out " + colorText(sample(shot, 320, 180)));

    // values handed over before the chain exists are applied when it is built
    p.setParameters({{"BRIGHT", 0.5}});
    p.refresh(preset);
    QTest::qWait(400);
    shot = p.grabFramebuffer();
    check(near(sample(shot, 320, 180), 60, 40, 20), "parameters set before the build apply to it " + colorText(sample(shot, 320, 180)));

    p.setParameters({});
    p.setSourceImage(picPath);
    p.refresh(preset);
    QTest::qWait(400);
    shot = p.grabFramebuffer();
    check(near(sample(shot, 200, 90), 255, 0, 0) && near(sample(shot, 200, 270), 0, 0, 255),
          "through a preset the picture is still the right way up");
}

void testDownload(AGSSetup &w, const QString &work) {
    std::puts("== get shaders: download, unpack, swap");
    Clicker clicker;
    FileServer srv;
    qputenv("AGSSETUP_SHADERS_URL", srv.url().toUtf8());
    const QString dest = AGSSetup::shadersHome();
    QDir(dest).removeRecursively();

    // 1. a good archive
    srv.body = makeTarball(work, "slang-shaders-master",
                           {{"crt/crt-test.slangp", "shaders = 0\n"}, {"crt/shaders/a.slang", "// a\n"}, {"README", "hi\n"}});
    w.lastShaderDir.clear();
    w.downloadShaders();
    check(srv.requests == 1, "it asked the server once");
    check(QFileInfo::exists(dest + "/crt/crt-test.slangp") && QFileInfo::exists(dest + "/crt/shaders/a.slang"),
          "the collection is unpacked into " + dest + " with the top folder stripped");
    check(!QFileInfo::exists(dest + ".new") && !QFileInfo::exists(dest + ".old") && !QFileInfo::exists(dest + ".download.tar.gz"),
          "no staging leftovers");
    check(w.lastShaderDir == dest && w.shaderStartDir() == dest, "the shader dialogs now open there");

    // 2. an update replaces the old copy wholesale
    writeFile(dest + "/stale-file", "old");
    srv.body = makeTarball(work, "slang-shaders-master", {{"crt/crt-test2.slangp", "shaders = 0\n"}});
    w.downloadShaders();
    check(QFileInfo::exists(dest + "/crt/crt-test2.slangp") && !QFileInfo::exists(dest + "/stale-file"),
          "a second download replaces the first");

    // 3. server error: the installed copy survives
    srv.status = 404;
    clicker.seen.clear();
    w.downloadShaders();
    check(clicker.seen.contains("Get shaders") && QFileInfo::exists(dest + "/crt/crt-test2.slangp"),
          "a failed download leaves the installed copy alone");
    check(!QFileInfo::exists(dest + ".download.tar.gz") && !QFileInfo::exists(dest + ".new"), "and cleans up after itself");

    // 4. an archive that is not the collection
    srv.status = 200;
    srv.body = makeTarball(work, "not-shaders", {{"readme.txt", "no presets here\n"}});
    w.downloadShaders();
    check(QFileInfo::exists(dest + "/crt/crt-test2.slangp"), "an archive without presets is not installed");
    srv.body = "this is not a tar file";
    w.downloadShaders();
    check(QFileInfo::exists(dest + "/crt/crt-test2.slangp") && !QFileInfo::exists(dest + ".new"), "garbage is not installed either");

    // 5. declining the question downloads nothing
    clicker.answers["Get shaders"] = QMessageBox::No;
    const int before = srv.requests;
    w.downloadShaders();
    check(srv.requests == before, "answering No does not touch the network");
    qunsetenv("AGSSETUP_SHADERS_URL");
}

} // namespace

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    const QString work = QDir::current().absoluteFilePath("work");
    QDir(work).removeRecursively();
    QDir().mkpath(work);
    // keep every bit of state inside the work folder
    qputenv("XDG_CONFIG_HOME", (work + "/xdg-config").toUtf8());
    qputenv("XDG_DATA_HOME", (work + "/xdg-data").toUtf8());

    const bool haveLibra = QFileInfo::exists(QCoreApplication::applicationDirPath() + "/librashader.so");
    if (haveLibra) {
        const libra_instance_t &l = PreviewLibra();
        check(l.instance_loaded, "librashader.so beside the test binary loads");
    }

    AGSSetup w{QString()};
    w.show();
    testLibraryRemoveMissing(w, work);
    testLocate(w, work);
    testLocateOpenGameWithEdits(w, work);
    testLibraryCap(work);
    testParams(w, work);
    testListParams(work, haveLibra);
    testDownload(w, work);
    w.hide();
    testPreview(work, haveLibra);

    std::printf("\n%d checks, %d failed, %d skipped\n", g_checks, g_fails, g_skips);
    return g_fails ? 1 : 0;
}

#include "gui_test.moc"
