// agssetup.cpp - Native Linux setup + launcher for the librabridge AGS build.
// Uses Qt6 (no Python required). Build with:
//   qmake6 agssetup.pro && make        (or: make, make cmake)
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
//   Keys this tool does not manage are preserved exactly as they were.
// - The options cover everything the Windows setup program (winsetup.exe)
//   offers - graphics, sound, mouse, language, save folders, caches,
//   accessibility - plus what the engine reads but winsetup does not expose
//   (FPS counter, mouse control mode, script-OS override, ...) and this
//   project's own: the librashader preset, extra engine arguments, diagnostics.
//   Every key written here is one the engine's own parser reads
//   (Engine/main/config.cpp).
// - The librashader preset lives in a [librabridge] section (the engine
//   ignores sections it does not know) and reaches the engine through the
//   AGS_LIBRASHADER_PRESET environment variable.
//
// Usage: agssetup [game folder or game data file]
//        (with no argument, the last used game is preselected;
//         --launcher / -l are still accepted and ignored)

#include <QApplication>
#include <QWidget>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTabWidget>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QSlider>
#include <QPushButton>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QGroupBox>
#include <QMenu>
#include <QAction>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QScreen>
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
#include <functional>

namespace {

// Preferences of this tool itself (not of any game)
const char *PREFS_LAST_GAME_PATH = "Preferences/LastGamePath";
const char *PREFS_LAST_GAME_DIR = "Preferences/LastGameDirectory";
const char *PREFS_LAST_SHADER_DIR = "Preferences/LastShaderDirectory";
const char *PREFS_WINDOW_GEOMETRY = "Preferences/MainWindowGeometry";
const char *PREFS_CLOSE_ON_PLAY = "Preferences/CloseOnPlay";

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

    bool contains(const QString &section, const QString &key) const {
        for (const Section &s : sections) {
            if (s.name.compare(section, Qt::CaseInsensitive) != 0)
                continue;
            for (const Entry &e : s.entries)
                if (e.key.compare(key, Qt::CaseInsensitive) == 0)
                    return true;
        }
        return false;
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
        resize(660, 740);
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
    // One row per config option: which key it is, its default when the key is
    // missing, and how to move the value between the file and the widget.
    struct Binding {
        QString section;
        QString key;
        QString def;
        bool omitIfEmpty;                          // empty = "engine default": drop the key
        std::function<void(const QString &)> load; // config value -> widget
        std::function<QString()> save;             // widget -> config value
    };
    QVector<Binding> bindings;

    // Tool preferences
    QString lastGamePath;
    QString lastGameDir;
    QString lastShaderDir;
    QByteArray windowGeometry;
    bool closeOnPlay = true;

    // Current game
    QString currentGame; // normalized path of the selected game; empty = none/invalid
    QString cfgPath;     // per-game settings file (outside the game folder)
    QString logPath;     // engine output of the last launch
    QString previewPath; // scratch config used by diagnostics (current UI state)
    QString seedPath;    // the game's own acsetup.cfg (read-only)
    IniFile ini;         // full config of the current game

    // UI - game + frame
    QLineEdit *gamePath = nullptr;
    QLabel *gameStatus = nullptr;
    QLabel *engineStatus = nullptr;
    QWidget *settingsPane = nullptr;
    QPushButton *saveBtn = nullptr;
    QPushButton *playBtn = nullptr;

    // UI - options that other code needs to reach
    QComboBox *display = nullptr;
    QComboBox *driver = nullptr;
    QComboBox *filter = nullptr;
    QCheckBox *windowed = nullptr;
    QComboBox *windowMode = nullptr;
    QComboBox *windowScale = nullptr;
    QComboBox *fullscreenMode = nullptr;
    QComboBox *fullscreenScale = nullptr;
    QSpinBox *refresh = nullptr;
    QCheckBox *vsync = nullptr;
    QCheckBox *renderAtScreenRes = nullptr;
    QCheckBox *antialias = nullptr;
    QCheckBox *showFps = nullptr;

    QCheckBox *shaderEnabled = nullptr;
    QLineEdit *shaderPath = nullptr;
    QWidget *shaderPane = nullptr;

    QCheckBox *soundEnabled = nullptr;
    QComboBox *audioDriver = nullptr;
    QCheckBox *speechEnabled = nullptr;
    QComboBox *soundCache = nullptr;
    QSpinBox *soundStream = nullptr;

    QCheckBox *mouseEnabled = nullptr;
    QCheckBox *mouseAutoLock = nullptr;
    QSlider *mouseSpeed = nullptr;
    QLabel *mouseSpeedLabel = nullptr;
    QComboBox *mouseControlWhen = nullptr;
    QComboBox *mouseSpeedDef = nullptr;
    QComboBox *touchMode = nullptr;
    QCheckBox *touchRelative = nullptr;

    QComboBox *language = nullptr;
    QCheckBox *compressSaves = nullptr;
    QCheckBox *loadLatestSave = nullptr;

    QComboBox *speechSkip = nullptr;
    QComboBox *textSkip = nullptr;
    QSlider *textReadSpeed = nullptr;
    QLabel *textReadSpeedLabel = nullptr;
    QComboBox *speechMode = nullptr;
    QCheckBox *alwaysWaitText = nullptr;

    QComboBox *spriteCache = nullptr;
    QComboBox *textureCache = nullptr;
    QCheckBox *clearCache = nullptr;
    QCheckBox *background = nullptr;
    QCheckBox *noPlugins = nullptr;
    QComboBox *reportOs = nullptr;
    QCheckBox *upscale = nullptr;
    QCheckBox *newKeys = nullptr;
    QLineEdit *extraArgs = nullptr;
    QCheckBox *closeOnPlayBox = nullptr;

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
        closeOnPlay = prefs.value(PREFS_CLOSE_ON_PLAY, true).toBool();
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
        if (closeOnPlayBox)
            prefs.setValue(PREFS_CLOSE_ON_PLAY, closeOnPlayBox->isChecked());
    }

    // ----- option bindings (see struct Binding) -----

    void bindCheck(QCheckBox *w, const char *section, const char *key, bool def) {
        bindings.append({section, key, def ? "1" : "0", false,
            [w](const QString &v) { w->setChecked(v.trimmed().toInt() != 0); },
            [w]() { return QString(w->isChecked() ? "1" : "0"); }});
    }

    // Non-editable combo whose items carry the config value as data. A value
    // that is not in the list (a hand-edited config) is kept as an extra item
    // instead of being silently replaced.
    void bindCombo(QComboBox *w, const char *section, const char *key, const QString &def,
                   bool omitIfEmpty = false) {
        bindings.append({section, key, def, omitIfEmpty,
            [w](const QString &v) {
                for (int i = w->count() - 1; i >= 0; --i)
                    if (w->itemData(i, Qt::UserRole + 1).toBool())
                        w->removeItem(i);
                int i = w->findData(v, Qt::UserRole, Qt::MatchFixedString);
                if (i < 0) {
                    w->addItem(v + " (custom)", v);
                    i = w->count() - 1;
                    w->setItemData(i, true, Qt::UserRole + 1);
                }
                w->setCurrentIndex(i);
            },
            [w]() { return w->currentData().toString(); }});
    }

    void bindEditCombo(QComboBox *w, const char *section, const char *key, const QString &def,
                       bool omitIfEmpty = false) {
        bindings.append({section, key, def, omitIfEmpty,
            [w](const QString &v) { w->setCurrentText(v); },
            [w]() { return w->currentText().trimmed(); }});
    }

    void bindLine(QLineEdit *w, const char *section, const char *key, const QString &def,
                  bool omitIfEmpty = false) {
        bindings.append({section, key, def, omitIfEmpty,
            [w](const QString &v) { w->setText(v); },
            [w]() { return w->text().trimmed(); }});
    }

    void bindSpin(QSpinBox *w, const char *section, const char *key, int def) {
        bindings.append({section, key, QString::number(def), false,
            [w](const QString &v) { w->setValue(v.trimmed().toInt()); },
            [w]() { return QString::number(w->value()); }});
    }

    // Cache sizes are stored in KB; the combo offers round MB values.
    void bindCacheCombo(QComboBox *w, const char *section, const char *key, qulonglong defKb) {
        bindings.append({section, key, QString::number(defKb), false,
            [w](const QString &v) {
                for (int i = w->count() - 1; i >= 0; --i)
                    if (w->itemData(i, Qt::UserRole + 1).toBool())
                        w->removeItem(i);
                int i = w->findData(v);
                if (i < 0) {
                    const qulonglong kb = v.trimmed().toULongLong();
                    const QString text = (kb % 1024 == 0) ? QString("%1 MB (custom)").arg(kb / 1024)
                                                          : QString("%1 KB (custom)").arg(kb);
                    w->addItem(text, v);
                    i = w->count() - 1;
                    w->setItemData(i, true, Qt::UserRole + 1);
                }
                w->setCurrentIndex(i);
            },
            [w]() { return w->currentData().toString(); }});
    }

    static void fillCacheCombo(QComboBox *w, const QVector<int> &mbValues, bool withOff) {
        if (withOff)
            w->addItem("Off", QString::number(0));
        for (int mb : mbValues) {
            const QString text = (mb >= 1024 && mb % 256 == 0)
                ? (mb % 1024 == 0 ? QString("%1 GB").arg(mb / 1024) : QString("%1 GB").arg(mb / 1024.0, 0, 'f', 2))
                : QString("%1 MB").arg(mb);
            w->addItem(text, QString::number(qulonglong(mb) * 1024));
        }
    }

    // Slider with a fixed decimal scale: config value "1.5" <-> slider 15.
    void bindScaledSlider(QSlider *w, const char *section, const char *key, double def, double scale) {
        bindings.append({section, key, QString::number(def, 'f', 1), false,
            [w, scale](const QString &v) { w->setValue(qRound(v.trimmed().toDouble() * scale)); },
            [w, scale]() { return QString::number(w->value() / scale, 'f', 1); }});
    }

    void bindIntSlider(QSlider *w, const char *section, const char *key, int def) {
        bindings.append({section, key, QString::number(def), false,
            [w](const QString &v) { w->setValue(v.trimmed().toInt()); },
            [w]() { return QString::number(w->value()); }});
    }

    // "Use a custom folder" + path + Browse. Unchecked = key dropped (engine default).
    void addDirOption(QFormLayout *form, const QString &label, const char *section, const char *key,
                      const QString &tip) {
        QCheckBox *use = new QCheckBox("Use a custom folder", this);
        QLineEdit *edit = new QLineEdit(this);
        QPushButton *browse = new QPushButton("Browse...", this);
        use->setToolTip(tip);
        QHBoxLayout *row = new QHBoxLayout();
        row->addWidget(edit, 1);
        row->addWidget(browse);
        form->addRow(label, use);
        form->addRow(QString(), row);
        auto sync = [use, edit, browse]() {
            edit->setEnabled(use->isChecked());
            browse->setEnabled(use->isChecked());
        };
        connect(use, &QCheckBox::toggled, this, sync);
        connect(browse, &QPushButton::clicked, this, [this, edit]() {
            const QString d = QFileDialog::getExistingDirectory(this, "Choose a folder",
                edit->text().isEmpty() ? QDir::homePath() : edit->text());
            if (!d.isEmpty())
                edit->setText(d);
        });
        bindings.append({section, key, QString(), true,
            [use, edit, sync](const QString &v) {
                use->setChecked(!v.isEmpty());
                edit->setText(v);
                sync();
            },
            [use, edit]() { return use->isChecked() ? edit->text().trimmed() : QString(); }});
    }

    // ----- UI construction -----

    QGroupBox *makeGroup(const QString &title, QFormLayout **formOut) {
        QGroupBox *box = new QGroupBox(title, this);
        QFormLayout *form = new QFormLayout(box);
        form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        *formOut = form;
        return box;
    }

    QComboBox *makeCombo(const QVector<QPair<QString, QString>> &items) {
        QComboBox *c = new QComboBox(this);
        for (const auto &it : items)
            c->addItem(it.first, it.second);
        return c;
    }

    QComboBox *makeEditableCombo(const QStringList &items) {
        QComboBox *c = new QComboBox(this);
        c->setEditable(true);
        c->setInsertPolicy(QComboBox::NoInsert);
        c->addItems(items);
        return c;
    }

    QWidget *buildGraphicsTab() {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QFormLayout *f = nullptr;

        lay->addWidget(makeGroup("Display", &f));
        display = new QComboBox(this);
        display->addItem("Default", "0");
        const QList<QScreen *> screens = QGuiApplication::screens();
        for (int i = 0; i < screens.size(); ++i)
            display->addItem(QString("%1: %2 (%3x%4)").arg(i + 1).arg(screens[i]->name())
                                 .arg(screens[i]->geometry().width()).arg(screens[i]->geometry().height()),
                             QString::number(i + 1));
        display->setToolTip("Monitor the game starts on.");
        bindCombo(display, "graphics", "display", "0");
        f->addRow("Monitor:", display);

        driver = makeCombo({{"OpenGL (required for shaders)", "OGL"}, {"Software", "Software"}});
        driver->setToolTip("Graphics renderer. Shader presets only work with OpenGL.");
        bindCombo(driver, "graphics", "driver", "OGL");
        f->addRow("Renderer:", driver);

        filter = makeCombo({{"Nearest-neighbour", "StdScale"}, {"Linear interpolation", "Linear"}});
        filter->setToolTip("How the game image is scaled to the screen. With a shader preset the\n"
                           "shader takes over the scaling.");
        bindCombo(filter, "graphics", "filter", "StdScale");
        f->addRow("Scaling filter:", filter);

        connect(driver, &QComboBox::currentIndexChanged, this, &AGSSetup::updateControlStates);

        lay->addWidget(makeGroup("Window", &f));
        windowed = new QCheckBox("Start windowed", this);
        bindCheck(windowed, "graphics", "windowed", false);
        f->addRow(windowed);
        windowMode = makeEditableCombo({"default", "native", "x2", "x3", "x4", "x5", "x6"});
        windowMode->setToolTip("Window size: default, native (game resolution), xN (integer scale,\n"
                               "e.g. x3) or WxH (e.g. 1280x720).");
        bindEditCombo(windowMode, "graphics", "window", "default");
        f->addRow("Window size:", windowMode);
        windowScale = makeCombo({{"Round (integer steps)", "round"}, {"Proportional (keep aspect)", "proportional"},
                                 {"Stretch (fill window)", "stretch"}});
        bindCombo(windowScale, "graphics", "game_scale_win", "round");
        f->addRow("Window scaling:", windowScale);

        lay->addWidget(makeGroup("Fullscreen", &f));
        fullscreenMode = makeEditableCombo({"default", "full_window", "desktop", "native", "x2", "x3", "x4", "x5", "x6"});
        fullscreenMode->setToolTip("default, full_window (borderless window), desktop (desktop resolution),\n"
                                   "native (game resolution), xN (integer scale) or WxH (e.g. 1920x1080).");
        for (QScreen *screen : screens) { // real fullscreen at each monitor's own resolution
            const QString wh = QString("%1x%2").arg(screen->geometry().width()).arg(screen->geometry().height());
            if (fullscreenMode->findText(wh) < 0)
                fullscreenMode->addItem(wh);
        }
        bindEditCombo(fullscreenMode, "graphics", "fullscreen", "default");
        f->addRow("Fullscreen mode:", fullscreenMode);
        fullscreenScale = makeCombo({{"Proportional (keep aspect)", "proportional"}, {"Round (integer steps)", "round"},
                                     {"Stretch (fill screen)", "stretch"}});
        bindCombo(fullscreenScale, "graphics", "game_scale_fs", "proportional");
        f->addRow("Fullscreen scaling:", fullscreenScale);

        lay->addWidget(makeGroup("Rendering", &f));
        refresh = new QSpinBox(this);
        refresh->setRange(0, 500);
        refresh->setSpecialValueText("Default");
        refresh->setSuffix(" Hz");
        refresh->setToolTip("Preferred refresh rate for real fullscreen modes.");
        bindSpin(refresh, "graphics", "refresh", 0);
        f->addRow("Refresh rate:", refresh);
        vsync = new QCheckBox("Vertical sync", this);
        bindCheck(vsync, "graphics", "vsync", false);
        f->addRow(vsync);
        renderAtScreenRes = new QCheckBox("Render sprites at screen resolution", this);
        renderAtScreenRes->setToolTip("Draw sprites at the final screen resolution instead of the game's native one.");
        bindCheck(renderAtScreenRes, "graphics", "render_at_screenres", false);
        f->addRow(renderAtScreenRes);
        antialias = new QCheckBox("Smooth scaled sprites (antialias)", this);
        bindCheck(antialias, "graphics", "antialias", false);
        f->addRow(antialias);
        showFps = new QCheckBox("Show FPS counter", this);
        showFps->setToolTip("Draws the frames-per-second counter over the game.");
        bindCheck(showFps, "misc", "show_fps", false);
        f->addRow(showFps);

        lay->addStretch(1);
        return page;
    }

    QWidget *buildShaderTab() {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);

        shaderPane = new QGroupBox("Shader (librashader, OpenGL renderer only)", this);
        QVBoxLayout *sl = new QVBoxLayout(shaderPane);

        shaderEnabled = new QCheckBox("Use the shader preset below", this);
        shaderEnabled->setToolTip("Untick to play without the shader but keep the preset selected.");
        bindCheck(shaderEnabled, "librabridge", "shader_enabled", true);
        sl->addWidget(shaderEnabled);

        QHBoxLayout *row = new QHBoxLayout();
        shaderPath = new QLineEdit(this);
        shaderPath->setPlaceholderText("None - plain output");
        QPushButton *browseBtn = new QPushButton("Browse...", this);
        QPushButton *clearBtn = new QPushButton("Clear", this);
        connect(browseBtn, &QPushButton::clicked, this, &AGSSetup::browseShader);
        connect(clearBtn, &QPushButton::clicked, shaderPath, &QLineEdit::clear);
        row->addWidget(shaderPath, 1);
        row->addWidget(browseBtn);
        row->addWidget(clearBtn);
        sl->addLayout(row);
        bindings.append({"librabridge", "shader_preset", QString(), true,
            [this](const QString &v) { shaderPath->setText(v); },
            [this]() { return shaderPath->text().trimmed(); }});

        QPushButton *dirBtn = new QPushButton("Browse Shader Directory...", this);
        connect(dirBtn, &QPushButton::clicked, this, &AGSSetup::showShaderBrowser);
        sl->addWidget(dirBtn);

        QLabel *note = new QLabel(
            "Presets are RetroArch .slangp / .glslp files. Requires the engine build shipped with "
            "librabridge. The preset is ignored when the renderer is Software. If a preset cannot be "
            "built, the game starts without shaders and the reason is written to the engine log "
            "(Diagnostics > View last engine log).", this);
        note->setWordWrap(true);
        sl->addWidget(note);

        lay->addWidget(shaderPane);
        lay->addStretch(1);
        return page;
    }

    QWidget *buildAudioTab() {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QFormLayout *f = nullptr;

        lay->addWidget(makeGroup("Sound", &f));
        soundEnabled = new QCheckBox("Enable sound", this);
        bindCheck(soundEnabled, "sound", "enabled", true);
        f->addRow(soundEnabled);
        speechEnabled = new QCheckBox("Use the voice pack (speech.vox) when the game has one", this);
        bindCheck(speechEnabled, "sound", "usespeech", true);
        f->addRow(speechEnabled);
        audioDriver = makeEditableCombo({"", "default", "none", "pulseaudio", "pipewire", "alsa", "jack",
                                         "sndio", "disk", "dummy"});
        audioDriver->setToolTip("Audio backend. Empty/default lets SDL choose; \"none\" turns audio off.\n"
                                "Any SDL audio driver name can be typed in.");
        bindEditCombo(audioDriver, "sound", "driver", QString(), true);
        f->addRow("Audio driver:", audioDriver);

        lay->addWidget(makeGroup("Sound cache", &f));
        soundCache = new QComboBox(this);
        fillCacheCombo(soundCache, {16, 32, 64, 128}, true);
        soundCache->setToolTip("Memory used to keep small sounds decoded. Currently meant only for small sounds.");
        bindCacheCombo(soundCache, "sound", "cache_size", 32 * 1024);
        f->addRow("Cache size:", soundCache);
        soundStream = new QSpinBox(this);
        soundStream->setRange(0, 1048576);
        soundStream->setSingleStep(256);
        soundStream->setSuffix(" KB");
        soundStream->setToolTip("Sounds smaller than this are loaded into memory at once; bigger ones are streamed.");
        bindSpin(soundStream, "sound", "stream_threshold", 1024);
        f->addRow("Load-at-once threshold:", soundStream);

        lay->addStretch(1);
        return page;
    }

    QWidget *buildControlsTab() {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QFormLayout *f = nullptr;

        lay->addWidget(makeGroup("Mouse", &f));
        mouseEnabled = new QCheckBox("Enable the mouse", this);
        bindCheck(mouseEnabled, "mouse", "enabled", true);
        f->addRow(mouseEnabled);
        mouseAutoLock = new QCheckBox("Lock the mouse to the window automatically", this);
        bindCheck(mouseAutoLock, "mouse", "auto_lock", false);
        f->addRow(mouseAutoLock);

        mouseSpeed = new QSlider(Qt::Horizontal, this);
        mouseSpeed->setRange(1, 100); // 0.1x .. 10.0x, same range as winsetup
        mouseSpeedLabel = new QLabel(this);
        mouseSpeedLabel->setMinimumWidth(48);
        connect(mouseSpeed, &QSlider::valueChanged, this, [this](int v) {
            mouseSpeedLabel->setText(QString("%1x").arg(v / 10.0, 0, 'f', 1));
        });
        bindScaledSlider(mouseSpeed, "mouse", "speed", 1.0, 10.0);
        QHBoxLayout *speedRow = new QHBoxLayout();
        speedRow->addWidget(mouseSpeed, 1);
        speedRow->addWidget(mouseSpeedLabel);
        f->addRow("Speed:", speedRow);

        mouseControlWhen = makeCombo({{"In fullscreen only", "fullscreen"}, {"Always", "always"}, {"Never", "never"}});
        mouseControlWhen->setToolTip("When the engine takes control of the mouse (custom speed, keeping the\n"
                                     "cursor in the window) instead of leaving it to the system.");
        bindCombo(mouseControlWhen, "mouse", "control_when", "fullscreen");
        f->addRow("Engine mouse control:", mouseControlWhen);
        mouseSpeedDef = makeCombo({{"Relative to the current display", "current_display"}, {"Absolute", "absolute"}});
        mouseSpeedDef->setToolTip("How the speed value above is interpreted.");
        bindCombo(mouseSpeedDef, "mouse", "speed_def", "current_display");
        f->addRow("Speed unit:", mouseSpeedDef);

        lay->addWidget(makeGroup("Touch screens", &f));
        touchMode = makeCombo({{"One finger", "one_finger"}, {"Two fingers", "two_fingers"}, {"Off", "off"}});
        bindCombo(touchMode, "touch", "emul_mouse_mode", "one_finger");
        f->addRow("Touch as mouse:", touchMode);
        touchRelative = new QCheckBox("Relative motion (like a touchpad)", this);
        bindCheck(touchRelative, "touch", "emul_mouse_relative", false);
        f->addRow(touchRelative);

        lay->addStretch(1);
        return page;
    }

    QWidget *buildGameTab() {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QFormLayout *f = nullptr;

        lay->addWidget(makeGroup("Language", &f));
        language = new QComboBox(this);
        language->setToolTip("Translations (.tra files) found in the game folder.");
        bindCombo(language, "language", "translation", QString(), true);
        f->addRow("Translation:", language);

        lay->addWidget(makeGroup("Saved games and files", &f));
        addDirOption(f, "Save games folder:", "misc", "user_data_dir",
                     "Where saved games and the game's own files are written.\nUnticked = the engine's default.");
        addDirOption(f, "Shared data folder:", "misc", "shared_data_dir",
                     "Where files shared between games are written.\nUnticked = the engine's default.");
        compressSaves = new QCheckBox("Compress saved games", this);
        bindCheck(compressSaves, "misc", "compress_saves", true);
        f->addRow(compressSaves);
        loadLatestSave = new QCheckBox("Load the latest saved game on start", this);
        bindCheck(loadLatestSave, "misc", "load_latest_save", false);
        f->addRow(loadLatestSave);

        lay->addStretch(1);
        return page;
    }

    QWidget *buildAccessTab() {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QFormLayout *f = nullptr;

        lay->addWidget(makeGroup("Accessibility", &f));
        const QVector<QPair<QString, QString>> skipItems = {
            {"Game default", "default"}, {"Any input", "input"}, {"Any input or timeout", "any"}, {"Timeout only", "time"}};
        speechSkip = makeCombo(skipItems);
        bindCombo(speechSkip, "access", "speechskip", "default");
        f->addRow("Skip speech with:", speechSkip);
        textSkip = makeCombo(skipItems);
        bindCombo(textSkip, "access", "textskip", "default");
        f->addRow("Skip text with:", textSkip);

        textReadSpeed = new QSlider(Qt::Horizontal, this);
        textReadSpeed->setRange(0, 30); // 0 = game default; 30 = twice the default AGS speed
        textReadSpeedLabel = new QLabel(this);
        textReadSpeedLabel->setMinimumWidth(96);
        connect(textReadSpeed, &QSlider::valueChanged, this, [this](int v) {
            textReadSpeedLabel->setText(v == 0 ? QString("Game default") : QString("%1").arg(v));
        });
        textReadSpeed->setToolTip("Reading speed used to time text on screen. 0 keeps the game's own value.");
        bindIntSlider(textReadSpeed, "access", "textreadspeed", 0);
        QHBoxLayout *speedRow = new QHBoxLayout();
        speedRow->addWidget(textReadSpeed, 1);
        speedRow->addWidget(textReadSpeedLabel);
        f->addRow("Text reading speed:", speedRow);

        speechMode = makeCombo({{"Game default", "default"}, {"Text only", "text"}, {"Voice only", "voice"},
                                {"Voice and text", "textvoice"}});
        bindCombo(speechMode, "access", "speechmode", "default");
        f->addRow("Speech output:", speechMode);
        alwaysWaitText = new QCheckBox("Always wait for text (never auto-advance)", this);
        bindCheck(alwaysWaitText, "access", "alwayswaittext", false);
        f->addRow(alwaysWaitText);

        lay->addStretch(1);
        return page;
    }

    QWidget *buildAdvancedTab() {
        QWidget *page = new QWidget(this);
        QVBoxLayout *lay = new QVBoxLayout(page);
        QFormLayout *f = nullptr;

        lay->addWidget(makeGroup("Memory", &f));
        const QVector<int> mb = {16, 32, 64, 128, 256, 384, 512, 640, 768, 896, 1024, 1280, 1536, 1792, 2048};
        spriteCache = new QComboBox(this);
        fillCacheCombo(spriteCache, mb, false);
        bindCacheCombo(spriteCache, "graphics", "sprite_cache_size", 128 * 1024);
        f->addRow("Sprite cache:", spriteCache);
        textureCache = new QComboBox(this);
        fillCacheCombo(textureCache, mb, false);
        bindCacheCombo(textureCache, "graphics", "texture_cache_size", 128 * 1024);
        f->addRow("Texture cache:", textureCache);
        clearCache = new QCheckBox("Clear caches on every room change (low-memory systems)", this);
        bindCheck(clearCache, "misc", "clear_cache_on_room_change", false);
        f->addRow(clearCache);

        lay->addWidget(makeGroup("Behaviour", &f));
        background = new QCheckBox("Keep running in the background", this);
        bindCheck(background, "misc", "background", false);
        f->addRow(background);

        lay->addWidget(makeGroup("Compatibility (for old or Windows-only games)", &f));
        noPlugins = new QCheckBox("Do not load plugins", this);
        noPlugins->setToolTip("Some games' Windows-only plugins cannot be loaded on Linux.");
        bindCheck(noPlugins, "override", "noplugins", false);
        f->addRow(noPlugins);
        reportOs = makeCombo({{"Automatic", ""}, {"Windows", "win"}, {"Linux", "linux"}, {"macOS", "mac"},
                              {"DOS", "dos"}, {"Android", "android"}, {"iOS", "ios"}, {"PSP", "psp"},
                              {"Web", "web"}, {"FreeBSD", "freebsd"}});
        reportOs->setToolTip("The operating system reported to the game's scripts. Some games only\n"
                             "enable features when they think they run on Windows.");
        bindCombo(reportOs, "override", "os", QString(), true);
        f->addRow("Report OS to scripts as:", reportOs);
        upscale = new QCheckBox("Run low-resolution games in a high-resolution mode", this);
        bindCheck(upscale, "override", "upscale", false);
        f->addRow(upscale);
        newKeys = new QCheckBox("Use the new keyboard handling in old games", this);
        bindCheck(newKeys, "override", "new_key_mode", false);
        f->addRow(newKeys);

        lay->addWidget(makeGroup("Launcher", &f));
        extraArgs = new QLineEdit(this);
        extraArgs->setPlaceholderText("e.g. --startr 3 --script-log");
        extraArgs->setToolTip("Extra command-line arguments passed to the engine (see: ags --help).");
        bindings.append({"librabridge", "extra_args", QString(), true,
            [this](const QString &v) { extraArgs->setText(v); },
            [this]() { return extraArgs->text().trimmed(); }});
        f->addRow("Extra engine arguments:", extraArgs);
        closeOnPlayBox = new QCheckBox("Close this window when the game starts", this);
        closeOnPlayBox->setChecked(closeOnPlay);
        f->addRow(closeOnPlayBox);

        lay->addStretch(1);
        return page;
    }

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

        QTabWidget *tabs = new QTabWidget(settingsPane);
        tabs->addTab(buildGraphicsTab(), "Graphics");
        tabs->addTab(buildShaderTab(), "Shader");
        tabs->addTab(buildAudioTab(), "Audio");
        tabs->addTab(buildControlsTab(), "Controls");
        tabs->addTab(buildGameTab(), "Game");
        tabs->addTab(buildAccessTab(), "Accessibility");
        tabs->addTab(buildAdvancedTab(), "Advanced");
        paneLayout->addWidget(tabs, 1);

        QHBoxLayout *toolRow = new QHBoxLayout();
        QPushButton *resetBtn = new QPushButton("Reset to game defaults", settingsPane);
        resetBtn->setToolTip("Reload the options from the game's own acsetup.cfg (or the defaults).\n"
                             "Nothing is saved until you press Save.");
        connect(resetBtn, &QPushButton::clicked, this, &AGSSetup::resetToGameDefaults);
        QPushButton *diagBtn = new QPushButton("Diagnostics", settingsPane);
        QMenu *diagMenu = new QMenu(diagBtn);
        connect(diagMenu->addAction("View last engine log"), &QAction::triggered, this, &AGSSetup::viewLog);
        connect(diagMenu->addAction("Configuration the engine will read (--tell-config)"), &QAction::triggered,
                this, [this]() { engineInfo("--tell-config", "Engine configuration"); });
        connect(diagMenu->addAction("Game data location (--tell-data)"), &QAction::triggered,
                this, [this]() { engineInfo("--tell-data", "Game data"); });
        connect(diagMenu->addAction("Game properties (--tell-gameproperties)"), &QAction::triggered,
                this, [this]() { engineInfo("--tell-gameproperties", "Game properties"); });
        diagBtn->setMenu(diagMenu);
        toolRow->addWidget(resetBtn);
        toolRow->addWidget(diagBtn);
        toolRow->addStretch(1);
        paneLayout->addLayout(toolRow);

        root->addWidget(settingsPane, 1);

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

    // Controls that only make sense together with another one.
    void updateControlStates() {
        if (!driver || !shaderPane)
            return;
        shaderPane->setEnabled(driver->currentData().toString() == "OGL");
        // Slider labels follow valueChanged, which does not fire when a loaded
        // value equals the slider's current one.
        mouseSpeedLabel->setText(QString("%1x").arg(mouseSpeed->value() / 10.0, 0, 'f', 1));
        textReadSpeedLabel->setText(textReadSpeed->value() == 0 ? QString("Game default")
                                                                : QString::number(textReadSpeed->value()));
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
        previewPath.clear();
        seedPath.clear();
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
        previewPath = QDir(configDir()).filePath(key + ".preview.cfg");
        seedPath = QDir(gameDirOf(currentGame)).filePath("acsetup.cfg");
        loadGameConfig();
        updateEnabledState();
        return true;
    }

    // Translations are the .tra files that sit in the game's folder.
    void populateLanguages() {
        language->clear();
        language->addItem("Game default", "");
        const QStringList tra = QDir(gameDirOf(currentGame)).entryList({"*.tra", "*.TRA"}, QDir::Files, QDir::Name);
        for (const QString &name : tra) {
            const QString base = QFileInfo(name).completeBaseName();
            if (language->findData(base) < 0)
                language->addItem(base, base);
        }
    }

    // Widgets <- ini
    void populateWidgets() {
        populateLanguages();
        for (const Binding &b : bindings)
            b.load(ini.value(b.section, b.key, b.def));
        updateControlStates();
    }

    void loadGameConfig() {
        ini = IniFile();
        QString origin;
        if (ini.load(cfgPath)) {
            origin = "Saved settings loaded.";
        } else {
            // First time for this game: start from its own acsetup.cfg, read-only.
            origin = ini.load(seedPath) ? "Started from the game's acsetup.cfg (left untouched)."
                                        : "No acsetup.cfg in the game folder; starting from defaults.";
        }
        normalizeLegacyValues();
        populateWidgets();
        gameStatus->setText(origin + "\nSettings are kept in " + cfgPath +
                            "\n(nothing is written to the game folder)");
    }

    // Values older engines wrote that current ones still accept under a new name.
    void normalizeLegacyValues() {
        for (const char *key : {"game_scale_fs", "game_scale_win"})
            if (ini.value("graphics", key).compare("max_round", Qt::CaseInsensitive) == 0)
                ini.setValue("graphics", key, "round");
    }

    void resetToGameDefaults() {
        if (currentGame.isEmpty())
            return;
        if (QMessageBox::question(this, "Reset to game defaults",
                "Reload every option from the game's own configuration?\n"
                "Unsaved changes are lost; nothing is saved until you press Save.") != QMessageBox::Yes)
            return;
        ini = IniFile();
        const bool haveSeed = ini.load(seedPath);
        normalizeLegacyValues();
        populateWidgets();
        gameStatus->setText(QString(haveSeed ? "Options reloaded from the game's acsetup.cfg."
                                             : "No acsetup.cfg in the game folder; options reset to defaults.") +
                            "\nNot saved yet.");
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

    // ----- saving -----

    // ini <- widgets (into any IniFile, so diagnostics can use a scratch copy)
    void applyWidgetsTo(IniFile &target) const {
        for (const Binding &b : bindings) {
            const QString v = b.save();
            if (b.omitIfEmpty && v.isEmpty())
                target.removeValue(b.section, b.key);
            else
                target.setValue(b.section, b.key, v);
        }
        target.setValue("librabridge", "game_path", currentGame);
    }

    bool save() {
        if (currentGame.isEmpty())
            return false;
        if (!QDir().mkpath(configDir())) {
            QMessageBox::critical(this, "Save failed",
                                  "Could not create the settings folder:\n" + configDir());
            return false;
        }
        applyWidgetsTo(ini);
        if (!ini.save(cfgPath)) {
            QMessageBox::critical(this, "Save failed", "Could not write:\n" + cfgPath);
            return false;
        }
        savePreferences();
        return true;
    }

    // ----- launching and diagnostics -----

    QProcessEnvironment engineEnvironment(bool withShader) const {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // The librashader bridge dlopen()s "librashader.so" by bare name, so the
        // folder holding the shipped library must be on the loader's search path.
        const QString appDir = QCoreApplication::applicationDirPath();
        const QString ld = env.value("LD_LIBRARY_PATH");
        env.insert("LD_LIBRARY_PATH", ld.isEmpty() ? appDir : appDir + ":" + ld);
        const QString preset = shaderPath->text().trimmed();
        if (withShader && !preset.isEmpty())
            env.insert("AGS_LIBRASHADER_PRESET", preset);
        else
            env.remove("AGS_LIBRASHADER_PRESET");
        return env;
    }

    bool shaderWanted() const {
        return shaderEnabled->isChecked() && !shaderPath->text().trimmed().isEmpty() &&
               driver->currentData().toString() == "OGL";
    }

    QStringList extraEngineArgs() const {
        const QString text = extraArgs->text().trimmed();
        return text.isEmpty() ? QStringList() : QProcess::splitCommand(text);
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

        const bool useShader = shaderWanted();
        const QString preset = shaderPath->text().trimmed();
        if (useShader && !QFileInfo::exists(preset)) {
            QMessageBox::warning(this, "Shader preset not found",
                                 "The shader preset does not exist:\n" + preset);
            return false;
        }

        QProcess proc;
        proc.setProgram(engine);
        // --conf makes the engine read ONLY our per-game file; the game path is
        // passed as-is and can be a folder or a data file.
        QStringList args = {"--conf", cfgPath};
        args += extraEngineArgs();
        args << currentGame;
        proc.setArguments(args);
        proc.setWorkingDirectory(gameDirOf(currentGame));
        proc.setProcessEnvironment(engineEnvironment(useShader));
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
        if (launch() && closeOnPlayBox->isChecked())
            QApplication::quit();
    }

    void showText(const QString &title, const QString &text) {
        QDialog dlg(this);
        dlg.setWindowTitle(title);
        QVBoxLayout *l = new QVBoxLayout(&dlg);
        QPlainTextEdit *edit = new QPlainTextEdit(&dlg);
        edit->setReadOnly(true);
        edit->setLineWrapMode(QPlainTextEdit::NoWrap);
        edit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        edit->setPlainText(text);
        l->addWidget(edit, 1);
        QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Close, &dlg);
        connect(bb, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
        l->addWidget(bb);
        dlg.resize(820, 520);
        dlg.exec();
    }

    void viewLog() {
        QFile f(logPath);
        if (logPath.isEmpty() || !f.open(QIODevice::ReadOnly)) {
            showText("Engine log", "No engine log yet: it is written when the game is started from here.");
            return;
        }
        const qint64 keep = 256 * 1024; // the tail is what matters
        if (f.size() > keep)
            f.seek(f.size() - keep);
        showText("Engine log - " + logPath, QString::fromUtf8(f.readAll()));
    }

    // Runs the engine with one of its --tell-* switches against the CURRENT
    // (possibly unsaved) options and returns what it printed. Nothing is
    // launched: these switches print and exit.
    QString runEngineInfo(const QString &tellSwitch, bool *ok) {
        *ok = false;
        QString err;
        const QString engine = bundledEngine(&err);
        if (engine.isEmpty())
            return err;
        if (!QDir().mkpath(configDir()))
            return "Could not create " + configDir();
        IniFile scratch = ini;
        applyWidgetsTo(scratch);
        if (!scratch.save(previewPath))
            return "Could not write " + previewPath;

        QProcess proc;
        proc.setProcessChannelMode(QProcess::MergedChannels);
        proc.setWorkingDirectory(gameDirOf(currentGame));
        proc.setProcessEnvironment(engineEnvironment(shaderWanted()));
        proc.start(engine, {"--conf", previewPath, tellSwitch, currentGame});
        if (!proc.waitForStarted(5000))
            return "Could not start the engine: " + proc.errorString();
        if (!proc.waitForFinished(20000)) {
            proc.kill();
            proc.waitForFinished(2000);
            return QString::fromUtf8(proc.readAll()) + "\n[stopped after 20 seconds]";
        }
        *ok = true;
        return QString::fromUtf8(proc.readAll());
    }

    void engineInfo(const QString &tellSwitch, const QString &title) {
        if (currentGame.isEmpty())
            return;
        bool ok = false;
        const QString out = runEngineInfo(tellSwitch, &ok);
        showText(title + " - " + tellSwitch, out.trimmed().isEmpty() ? "(the engine printed nothing)" : out);
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
