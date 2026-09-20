// agssetup.cpp - Native Linux setup + launcher for the librabridge AGS build.
// Uses Qt6 (no Python required). Build with:
//   qmake6 agssetup.pro && make
// or with CMake (see CMakeLists.txt).
//
// How it works:
// - You point it at EXISTING game data (a game folder, or a .ags / .exe /
//   ac2game.dat file) with the Browse button. Nothing is copied and nothing
//   is written into the game's folder.
// - The game is always run with the engine that ships with librabridge: the
//   "ags" binary sitting next to this executable (librashader.so lives there
//   too). No engine is searched in the game folder or in system paths.
// - Settings are stored per game in ~/.config/agssetup/games/<name>-<hash>.cfg.
//   The first time a game is selected, that file is seeded from the game's own
//   acsetup.cfg (read-only). The engine is started with "--conf <that file>",
//   which makes it read that single file instead of any config in the game dir.
// - The librashader preset is kept in the same file, in a [librabridge]
//   section (the engine ignores sections it does not know), and is passed to
//   the engine through the AGS_LIBRASHADER_PRESET environment variable.
//
// Usage: agssetup [game folder or game data file]
//        (with no argument, the last used game is preselected;
//         --launcher / -l are still accepted and ignored)

#include <QApplication>
#include <QWidget>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QGroupBox>
#include <QMenu>
#include <QAction>
#include <QSettings>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QCloseEvent>
#include <QVector>

namespace {

// Values below were checked against the engine's own parser (Engine/main/config.cpp).
const QStringList GRAPHICS_DRIVERS = {"OGL", "Software"};
// [graphics] fullscreen: "default", "full_window" (borderless window), "desktop",
// "native", an integer scale "xN" or an explicit "WxH". The combo is editable so
// the last two can be typed in.
const QStringList FULLSCREEN_MODES = {"default", "full_window", "desktop", "native"};
// [graphics] game_scale_fs ("max_round" is a legacy alias of "round")
const QStringList SCALE_MODES = {"proportional", "round", "stretch"};

// Preferences of this tool itself (not of any game)
const char *PREFS_LAST_GAME_PATH = "Preferences/LastGamePath";
const char *PREFS_LAST_GAME_DIR = "Preferences/LastGameDirectory";
const char *PREFS_LAST_SHADER_DIR = "Preferences/LastShaderDirectory";
const char *PREFS_WINDOW_GEOMETRY = "Preferences/MainWindowGeometry";

// ---------------------------------------------------------------------------
// Minimal INI model. QSettings is deliberately not used for the engine config:
// it re-encodes values (commas become lists, escapes, quoting) and could damage
// keys we do not manage. This keeps every unknown section/key exactly as read.
// ---------------------------------------------------------------------------
class IniFile {
public:
    bool load(const QString &path) {
        sections.clear();
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            return false;
        QTextStream in(&f);
        QString current; // "" = keys found before the first [section]
        while (!in.atEnd()) {
            const QString line = in.readLine().trimmed();
            if (line.isEmpty() || line.startsWith('#') || line.startsWith(';'))
                continue;
            if (line.startsWith('[') && line.endsWith(']')) {
                current = line.mid(1, line.size() - 2).trimmed();
                findSection(current, true);
                continue;
            }
            const int eq = line.indexOf('=');
            if (eq <= 0)
                continue;
            setValue(current, line.left(eq).trimmed(), line.mid(eq + 1).trimmed());
        }
        return true;
    }

    bool save(const QString &path) const {
        QSaveFile f(path); // atomic: writes a temp file, then renames it
        if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
            return false;
        QTextStream out(&f);
        for (const Section &s : sections) {
            if (!s.name.isEmpty())
                out << '[' << s.name << "]\n";
            for (const Entry &e : s.entries)
                out << e.key << '=' << e.value << '\n';
            out << '\n';
        }
        out.flush();
        return f.commit();
    }

    QString value(const QString &section, const QString &key, const QString &def = QString()) const {
        for (const Section &s : sections) {
            if (s.name.compare(section, Qt::CaseInsensitive) != 0)
                continue;
            for (const Entry &e : s.entries)
                if (e.key.compare(key, Qt::CaseInsensitive) == 0)
                    return e.value;
        }
        return def;
    }

    void setValue(const QString &section, const QString &key, const QString &value) {
        Section *s = findSection(section, true);
        for (Entry &e : s->entries) {
            if (e.key.compare(key, Qt::CaseInsensitive) == 0) {
                e.value = value; // keep the spelling already in the file
                return;
            }
        }
        s->entries.append({key, value});
    }

    void removeValue(const QString &section, const QString &key) {
        Section *s = findSection(section, false);
        if (!s)
            return;
        for (int i = 0; i < s->entries.size(); ++i) {
            if (s->entries[i].key.compare(key, Qt::CaseInsensitive) == 0) {
                s->entries.removeAt(i);
                return;
            }
        }
    }

private:
    struct Entry { QString key; QString value; };
    struct Section { QString name; QVector<Entry> entries; };
    QVector<Section> sections;

    Section *findSection(const QString &name, bool create) {
        for (Section &s : sections)
            if (s.name.compare(name, Qt::CaseInsensitive) == 0)
                return &s;
        if (!create)
            return nullptr;
        sections.append({name, {}});
        return &sections.last();
    }
};

// ---------------------------------------------------------------------------
// Helpers about game locations
// ---------------------------------------------------------------------------

// Accepts what a user may paste or type: quotes, "~/..." and relative paths.
QString normalizeUserPath(QString s) {
    s = s.trimmed();
    if (s.size() >= 2 && ((s.startsWith('"') && s.endsWith('"')) || (s.startsWith('\'') && s.endsWith('\'') )))
        s = s.mid(1, s.size() - 2).trimmed();
    if (s == "~" || s.startsWith("~/"))
        s = QDir::homePath() + s.mid(1);
    if (s.isEmpty())
        return s;
    return QDir::cleanPath(QFileInfo(s).absoluteFilePath());
}

bool isDataFileName(const QString &name) {
    return name.endsWith(".ags", Qt::CaseInsensitive) ||
           name.endsWith(".exe", Qt::CaseInsensitive) ||
           name.compare("ac2game.dat", Qt::CaseInsensitive) == 0 ||
           name.compare("agsgame.dat", Qt::CaseInsensitive) == 0;
}

// The engine accepts either a data file or a folder in which it finds the data,
// either through the folder's acsetup.cfg or by scanning it (Engine/main/engine.cpp).
bool looksLikeGame(const QString &path, QString *why) {
    const QFileInfo fi(path);
    if (!fi.exists()) {
        *why = "This path does not exist.";
        return false;
    }
    if (fi.isFile())
        return true;
    if (fi.isDir()) {
        const QDir dir(path);
        if (dir.exists("acsetup.cfg"))
            return true;
        for (const QString &name : dir.entryList(QDir::Files))
            if (isDataFileName(name))
                return true;
        *why = "No AGS game data found in this folder (expected a .ags file, ac2game.dat, "
               "a game .exe or an acsetup.cfg).";
        return false;
    }
    *why = "Not a file or a folder.";
    return false;
}

QString gameDirOf(const QString &path) {
    const QFileInfo fi(path);
    return fi.isDir() ? fi.absoluteFilePath() : fi.absolutePath();
}

// Stable per-game identifier: readable name + hash of the real path, so two
// different games (or two copies of one) never share a settings file.
QString gameKey(const QString &path) {
    const QFileInfo fi(path);
    QString canon = fi.canonicalFilePath();
    if (canon.isEmpty())
        canon = fi.absoluteFilePath();
    QString base = fi.isDir() ? fi.fileName() : fi.completeBaseName();
    if (base.isEmpty())
        base = "game";
    base.replace(QRegularExpression("[^A-Za-z0-9._-]+"), "_");
    const QString hash = QString::fromLatin1(
        QCryptographicHash::hash(canon.toUtf8(), QCryptographicHash::Sha1).toHex().left(8));
    return base.left(40) + "-" + hash;
}

QString configDir() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           "/agssetup/games";
}

} // namespace


class AGSSetup : public QWidget {
    Q_OBJECT
public:
    explicit AGSSetup(const QString &initialGame, QWidget *parent = nullptr)
        : QWidget(parent) {
        setWindowTitle("AGS Setup");
        resize(460, 580);
        loadPreferences();
        buildUI();

        if (!windowGeometry.isEmpty())
            restoreGeometry(windowGeometry);

        const QString start = initialGame.isEmpty() ? lastGamePath : initialGame;
        if (!start.isEmpty()) {
            gamePath->setText(start);
            selectGame(start);
        } else {
            updateEnabledState();
        }
    }

private:
    // Tool preferences
    QString lastGamePath;
    QString lastGameDir;
    QString lastShaderDir;
    QByteArray windowGeometry;

    // Current game
    QString currentGame; // normalized path of the selected game; empty = none/invalid
    QString cfgPath;     // per-game settings file (outside the game folder)
    QString logPath;     // engine output of the last launch
    IniFile ini;         // full config of the current game

    // UI
    QLineEdit *gamePath = nullptr;
    QLabel *gameStatus = nullptr;
    QLabel *engineStatus = nullptr;
    QWidget *settingsPane = nullptr;
    QComboBox *driver = nullptr;
    QCheckBox *windowed = nullptr;
    QComboBox *fullscreenMode = nullptr;
    QComboBox *scaleFs = nullptr;
    QCheckBox *vsync = nullptr;
    QCheckBox *antialias = nullptr;
    QCheckBox *soundEnabled = nullptr;
    QCheckBox *speechEnabled = nullptr;
    QLineEdit *shaderPath = nullptr;
    QPushButton *saveBtn = nullptr;
    QPushButton *playBtn = nullptr;

    // ----- preferences of the tool -----

    QSettings makePrefs() const {
        return QSettings(QSettings::IniFormat, QSettings::UserScope, "AGSSetup", "agssetup");
    }

    void loadPreferences() {
        QSettings prefs = makePrefs();
        lastGamePath = prefs.value(PREFS_LAST_GAME_PATH, "").toString();
        lastGameDir = prefs.value(PREFS_LAST_GAME_DIR, "").toString();
        lastShaderDir = prefs.value(PREFS_LAST_SHADER_DIR, "").toString();
        windowGeometry = prefs.value(PREFS_WINDOW_GEOMETRY).toByteArray();
    }

    void savePreferences() {
        QSettings prefs = makePrefs();
        if (!currentGame.isEmpty())
            prefs.setValue(PREFS_LAST_GAME_PATH, currentGame);
        if (!lastGameDir.isEmpty())
            prefs.setValue(PREFS_LAST_GAME_DIR, lastGameDir);
        if (!lastShaderDir.isEmpty())
            prefs.setValue(PREFS_LAST_SHADER_DIR, lastShaderDir);
        prefs.setValue(PREFS_WINDOW_GEOMETRY, saveGeometry());
    }

    // ----- UI -----

    void buildUI() {
        QVBoxLayout *root = new QVBoxLayout(this);

        // Game data (existing files, referenced in place)
        QGroupBox *gameBox = new QGroupBox("Game", this);
        QVBoxLayout *gameLayout = new QVBoxLayout(gameBox);

        QHBoxLayout *gameRow = new QHBoxLayout();
        gamePath = new QLineEdit(this);
        gamePath->setPlaceholderText("Game folder or game data file (.ags, .exe, ac2game.dat)");
        QPushButton *browseGameBtn = new QPushButton("Browse...", this);
        QMenu *browseMenu = new QMenu(browseGameBtn);
        QAction *pickFile = browseMenu->addAction("Game data file (.ags, .exe, ac2game.dat)...");
        QAction *pickDir = browseMenu->addAction("Game folder...");
        connect(pickFile, &QAction::triggered, this, &AGSSetup::browseGameFile);
        connect(pickDir, &QAction::triggered, this, &AGSSetup::browseGameFolder);
        browseGameBtn->setMenu(browseMenu);
        connect(gamePath, &QLineEdit::editingFinished, this, &AGSSetup::onGamePathEdited);
        gameRow->addWidget(gamePath, 1);
        gameRow->addWidget(browseGameBtn);
        gameLayout->addLayout(gameRow);

        gameStatus = new QLabel(this);
        gameStatus->setWordWrap(true);
        gameStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        gameLayout->addWidget(gameStatus);

        engineStatus = new QLabel(this);
        engineStatus->setWordWrap(true);
        engineStatus->setTextInteractionFlags(Qt::TextSelectableByMouse);
        gameLayout->addWidget(engineStatus);
        refreshEngineStatus();

        root->addWidget(gameBox);

        // Everything below is per game and disabled until a valid game is selected
        settingsPane = new QWidget(this);
        QVBoxLayout *paneLayout = new QVBoxLayout(settingsPane);
        paneLayout->setContentsMargins(0, 0, 0, 0);

        // Graphics
        QGroupBox *gfxBox = new QGroupBox("Graphics", settingsPane);
        QFormLayout *gfxForm = new QFormLayout(gfxBox);

        driver = new QComboBox(gfxBox);
        driver->addItems(GRAPHICS_DRIVERS);
        windowed = new QCheckBox("Start windowed", gfxBox);
        fullscreenMode = new QComboBox(gfxBox);
        fullscreenMode->setEditable(true);
        fullscreenMode->setInsertPolicy(QComboBox::NoInsert);
        fullscreenMode->addItems(FULLSCREEN_MODES);
        fullscreenMode->setToolTip("default, full_window (borderless window), desktop, native,\n"
                                   "xN (integer scale, e.g. x3) or WxH (e.g. 1920x1080)");
        scaleFs = new QComboBox(gfxBox);
        scaleFs->addItems(SCALE_MODES);
        vsync = new QCheckBox("Vsync", gfxBox);
        antialias = new QCheckBox("Smooth scaled sprites (antialias)", gfxBox);

        gfxForm->addRow("Renderer:", driver);
        gfxForm->addRow(windowed);
        gfxForm->addRow("Fullscreen mode:", fullscreenMode);
        gfxForm->addRow("Fullscreen scaling:", scaleFs);
        gfxForm->addRow(vsync);
        gfxForm->addRow(antialias);
        paneLayout->addWidget(gfxBox);

        // Shader
        QGroupBox *shaderBox = new QGroupBox("Shader (librashader - OGL renderer only)", settingsPane);
        QVBoxLayout *shaderLayout = new QVBoxLayout(shaderBox);

        QHBoxLayout *shaderRow = new QHBoxLayout();
        shaderPath = new QLineEdit(shaderBox);
        shaderPath->setPlaceholderText("None - plain output");
        QPushButton *browseShaderBtn = new QPushButton("Browse...", shaderBox);
        QPushButton *clearShaderBtn = new QPushButton("Clear", shaderBox);
        QPushButton *shaderDirBtn = new QPushButton("Browse Shader Directory...", shaderBox);
        connect(browseShaderBtn, &QPushButton::clicked, this, &AGSSetup::browseShader);
        connect(clearShaderBtn, &QPushButton::clicked, shaderPath, &QLineEdit::clear);
        connect(shaderDirBtn, &QPushButton::clicked, this, &AGSSetup::showShaderBrowser);
        shaderRow->addWidget(shaderPath, 1);
        shaderRow->addWidget(browseShaderBtn);
        shaderRow->addWidget(clearShaderBtn);
        shaderLayout->addLayout(shaderRow);
        shaderLayout->addWidget(shaderDirBtn);

        QLabel *note = new QLabel(
            "Requires the engine build shipped with librabridge. "
            "Ignored when the renderer above is Software.", shaderBox);
        note->setWordWrap(true);
        shaderLayout->addWidget(note);
        paneLayout->addWidget(shaderBox);

        // Sound
        QGroupBox *soundBox = new QGroupBox("Sound", settingsPane);
        QFormLayout *soundForm = new QFormLayout(soundBox);
        soundEnabled = new QCheckBox("Enable sound", soundBox);
        speechEnabled = new QCheckBox("Enable voice speech", soundBox);
        soundForm->addRow(soundEnabled);
        soundForm->addRow(speechEnabled);
        paneLayout->addWidget(soundBox);

        root->addWidget(settingsPane);
        root->addStretch(1);

        // Buttons
        QHBoxLayout *btnRow = new QHBoxLayout();
        saveBtn = new QPushButton("Save", this);
        playBtn = new QPushButton("Save && Play", this);
        playBtn->setDefault(true);
        connect(saveBtn, &QPushButton::clicked, this, &AGSSetup::save);
        connect(playBtn, &QPushButton::clicked, this, &AGSSetup::saveAndPlay);
        btnRow->addStretch(1);
        btnRow->addWidget(saveBtn);
        btnRow->addWidget(playBtn);
        root->addLayout(btnRow);
    }

    void updateEnabledState() {
        const bool ok = !currentGame.isEmpty();
        settingsPane->setEnabled(ok);
        saveBtn->setEnabled(ok);
        playBtn->setEnabled(ok);
    }

    // ----- the engine that ships with librabridge -----

    // Returns the path of the bundled engine ("ags" next to this executable),
    // or an empty string with a reason in *err.
    QString bundledEngine(QString *err) const {
        const QString path = QDir(QCoreApplication::applicationDirPath()).filePath("ags");
        QFileInfo fi(path);
        if (!fi.exists() || !fi.isFile()) {
            *err = "The engine was not found: expected \"ags\" next to agssetup, in\n" +
                   QCoreApplication::applicationDirPath();
            return QString();
        }
        if (!fi.isExecutable()) {
            // Typically the executable bit is lost when a CI artifact is unpacked from a zip.
            QFile f(path);
            f.setPermissions(f.permissions() | QFileDevice::ExeOwner |
                             QFileDevice::ExeGroup | QFileDevice::ExeOther);
            fi.refresh();
            if (!fi.isExecutable()) {
                *err = "The engine exists but is not executable and its permissions could not be "
                       "changed:\n" + path;
                return QString();
            }
        }
        return path;
    }

    void refreshEngineStatus() {
        QString err;
        const QString engine = bundledEngine(&err);
        if (engine.isEmpty())
            engineStatus->setText("Engine: NOT FOUND. " + err);
        else
            engineStatus->setText("Engine: " + engine);
    }

    // ----- selecting a game -----

    void onGamePathEdited() {
        const QString text = gamePath->text().trimmed();
        if (text.isEmpty()) {
            clearGame("");
            return;
        }
        // Focus loss without a real change must not discard unsaved edits
        if (!currentGame.isEmpty() && normalizeUserPath(text) == currentGame)
            return;
        selectGame(text);
    }

    void clearGame(const QString &message) {
        currentGame.clear();
        cfgPath.clear();
        logPath.clear();
        ini = IniFile();
        gameStatus->setText(message);
        updateEnabledState();
    }

    bool selectGame(const QString &rawPath) {
        const QString path = normalizeUserPath(rawPath);
        QString why;
        if (path.isEmpty() || !looksLikeGame(path, &why)) {
            clearGame(why);
            return false;
        }
        currentGame = path;
        gamePath->setText(path);
        const QString key = gameKey(path);
        cfgPath = QDir(configDir()).filePath(key + ".cfg");
        logPath = QDir(configDir()).filePath(key + ".log");
        loadGameConfig();
        updateEnabledState();
        return true;
    }

    void loadGameConfig() {
        ini = IniFile();
        QString origin;
        if (ini.load(cfgPath)) {
            origin = "Saved settings loaded.";
        } else {
            // First time for this game: start from its own acsetup.cfg, read-only.
            const QString seed = QDir(gameDirOf(currentGame)).filePath("acsetup.cfg");
            origin = ini.load(seed) ? "Started from the game's acsetup.cfg (left untouched)."
                                    : "No acsetup.cfg in the game folder; starting from defaults.";
        }

        setComboText(driver, ini.value("graphics", "driver", "OGL"));
        windowed->setChecked(ini.value("graphics", "windowed", "0") == "1");
        fullscreenMode->setCurrentText(ini.value("graphics", "fullscreen", "default"));
        QString scale = ini.value("graphics", "game_scale_fs", "proportional");
        if (scale.compare("max_round", Qt::CaseInsensitive) == 0)
            scale = "round"; // legacy name the engine still accepts
        setComboText(scaleFs, scale);
        vsync->setChecked(ini.value("graphics", "vsync", "0") == "1");
        antialias->setChecked(ini.value("graphics", "antialias", "0") == "1");
        soundEnabled->setChecked(ini.value("sound", "enabled", "1") == "1");
        speechEnabled->setChecked(ini.value("sound", "usespeech", "1") == "1");
        shaderPath->setText(ini.value("librabridge", "shader_preset"));

        gameStatus->setText(origin + "\nSettings are kept in " + cfgPath +
                            "\n(nothing is written to the game folder)");
    }

    static void setComboText(QComboBox *combo, const QString &value) {
        const int index = combo->findText(value, Qt::MatchFixedString);
        if (index >= 0)
            combo->setCurrentIndex(index);
    }

    // ----- browse actions -----

    QString browseStartDir() const {
        if (!lastGameDir.isEmpty() && QFileInfo(lastGameDir).isDir())
            return lastGameDir;
        return QDir::homePath();
    }

    void browseGameFile() {
        const QString f = QFileDialog::getOpenFileName(
            this, "Choose the game data file", browseStartDir(),
            "AGS game data (*.ags *.exe ac2game.dat agsgame.dat);;All files (*)");
        if (f.isEmpty())
            return;
        lastGameDir = QFileInfo(f).absolutePath();
        selectGame(f);
    }

    void browseGameFolder() {
        const QString d = QFileDialog::getExistingDirectory(
            this, "Choose the game folder", browseStartDir(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (d.isEmpty())
            return;
        lastGameDir = QFileInfo(d).absolutePath();
        selectGame(d);
    }

    QString shaderStartDir() const {
        if (!lastShaderDir.isEmpty())
            return lastShaderDir;
        return QDir::homePath();
    }

    void browseShader() {
        const QString path = QFileDialog::getOpenFileName(
            this, "Choose a RetroArch shader preset", shaderStartDir(),
            "Shader presets (*.slangp *.glslp);;All files (*)");
        if (!path.isEmpty()) {
            shaderPath->setText(path);
            lastShaderDir = QFileInfo(path).path();
        }
    }

    void showShaderBrowser() {
        const QString dir = QFileDialog::getExistingDirectory(
            this, "Select Shader Directory", shaderStartDir(),
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);
        if (dir.isEmpty())
            return;
        const QString path = QFileDialog::getOpenFileName(
            this, "Choose a shader preset", dir,
            "Shader presets (*.slangp *.glslp);;All files (*)");
        if (!path.isEmpty()) {
            shaderPath->setText(path);
            lastShaderDir = QFileInfo(path).path();
        }
    }

    // ----- saving and launching -----

    void applyWidgetsToIni() {
        ini.setValue("graphics", "driver", driver->currentText());
        ini.setValue("graphics", "windowed", windowed->isChecked() ? "1" : "0");
        const QString fs = fullscreenMode->currentText().trimmed();
        ini.setValue("graphics", "fullscreen", fs.isEmpty() ? QString("default") : fs);
        ini.setValue("graphics", "game_scale_fs", scaleFs->currentText());
        ini.setValue("graphics", "vsync", vsync->isChecked() ? "1" : "0");
        ini.setValue("graphics", "antialias", antialias->isChecked() ? "1" : "0");
        ini.setValue("sound", "enabled", soundEnabled->isChecked() ? "1" : "0");
        ini.setValue("sound", "usespeech", speechEnabled->isChecked() ? "1" : "0");

        // Kept in the same file; the engine ignores sections it does not know.
        ini.setValue("librabridge", "game_path", currentGame);
        const QString preset = shaderPath->text().trimmed();
        if (preset.isEmpty())
            ini.removeValue("librabridge", "shader_preset");
        else
            ini.setValue("librabridge", "shader_preset", preset);
    }

    bool save() {
        if (currentGame.isEmpty())
            return false;
        if (!QDir().mkpath(configDir())) {
            QMessageBox::critical(this, "Save failed",
                                  "Could not create the settings folder:\n" + configDir());
            return false;
        }
        applyWidgetsToIni();
        if (!ini.save(cfgPath)) {
            QMessageBox::critical(this, "Save failed", "Could not write:\n" + cfgPath);
            return false;
        }
        savePreferences();
        return true;
    }

    bool launch() {
        QString err;
        const QString engine = bundledEngine(&err);
        if (engine.isEmpty()) {
            QMessageBox::warning(this, "Engine not found", err);
            return false;
        }
        QString why;
        if (!looksLikeGame(currentGame, &why)) {
            QMessageBox::warning(this, "Game not found",
                                 "The selected game is no longer available:\n" + currentGame);
            return false;
        }

        const QString preset = shaderPath->text().trimmed();
        const bool useShader = !preset.isEmpty() && driver->currentText() == "OGL";
        if (useShader && !QFileInfo::exists(preset)) {
            QMessageBox::warning(this, "Shader preset not found",
                                 "The shader preset does not exist:\n" + preset);
            return false;
        }

        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // The librashader bridge dlopen()s "librashader.so" by bare name, so the
        // folder holding the shipped library must be on the loader's search path.
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString ld = env.value("LD_LIBRARY_PATH");
        env.insert("LD_LIBRARY_PATH", ld.isEmpty() ? appDir : appDir + ":" + ld);
        if (useShader)
            env.insert("AGS_LIBRASHADER_PRESET", preset);
        else
            env.remove("AGS_LIBRASHADER_PRESET");

        QProcess proc;
        proc.setProgram(engine);
        // --conf makes the engine read ONLY our per-game file; the game path is
        // passed as-is and can be a folder or a data file.
        proc.setArguments({"--conf", cfgPath, currentGame});
        proc.setWorkingDirectory(gameDirOf(currentGame));
        proc.setProcessEnvironment(env);
        proc.setStandardOutputFile(logPath, QIODevice::Truncate);
        proc.setStandardErrorFile(logPath, QIODevice::Append);
        if (!proc.startDetached()) {
            QMessageBox::critical(this, "Launch failed",
                                  "Could not start the engine:\n" + engine + "\n\n" + proc.errorString());
            return false;
        }
        return true;
    }

    void saveAndPlay() {
        if (!save())
            return;
        if (launch())
            QApplication::quit();
    }

    void closeEvent(QCloseEvent *event) override {
        savePreferences();
        QWidget::closeEvent(event);
    }
};


int main(int argc, char *argv[]) {
    QApplication app(argc, argv);

    QString initialGame;
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == "--launcher" || args[i] == "-l")
            continue; // accepted for backward compatibility, no longer needed
        if (!args[i].startsWith('-'))
            initialGame = args[i];
    }

    // The window must outlive this scope: it has to still exist when
    // app.exec() runs.
    AGSSetup *win = new AGSSetup(initialGame);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->show();

    return app.exec();
}

#include "agssetup.moc"
