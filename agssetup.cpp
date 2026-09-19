// agssetup.cpp - Native Linux replacement for AGS's winsetup.exe and launcher
// Uses Qt6 (no Python required). Compile with:
//   qmake6 -project && qmake6 && make
// or with CMake (see CMakeLists.txt)
//
// Features:
// - Reads/writes acsetup.cfg
// - Allows selecting a librashader preset
// - Can launch games from any directory (launcher mode)
// - Shader directory browser with preset selection
//
// The preset path is stored in a sidecar file (.agssetup_shader_preset)
// and passed to the engine via AGS_LIBRASHADER_PRESET environment variable.

#include <QApplication>
#include <QWidget>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QSpinBox>
#include <QLineEdit>
#include <QPushButton>
#include <QFileDialog>
#include <QLabel>
#include <QMessageBox>
#include <QGroupBox>
#include <QSettings>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QTreeView>
#include <QFileSystemModel>
#include <QSplitter>

// Keys from Engine/main/config.cpp - preserve exact spelling
const QStringList GRAPHICS_DRIVERS = {"OGL", "Software"};
const QStringList WINDOW_MODES = {
    "default", "fullscreen", "fullscreen_borderless", 
    "fullscreen_desktop", "windowed"
};
const QStringList SCALE_MODES = {
    "max_round", "stretch", "proportional", "round"
};


class AGSSetup : public QWidget {
    Q_OBJECT
public:
    AGSSetup(const QString &initialDir, bool launcherMode = false, QWidget *parent = nullptr)
        : QWidget(parent), initialDir(initialDir), launcherMode(launcherMode) {
        
        // In launcher mode, start with file browser; otherwise load game dir
        if (launcherMode) {
            gameDir = initialDir;
            cfgPath = "";
            sidecarPath = "";
            setWindowTitle("AGS Launcher - Select Game");
        } else {
            gameDir = initialDir;
            cfgPath = QDir(gameDir).filePath("acsetup.cfg");
            sidecarPath = QDir(gameDir).filePath(".agssetup_shader_preset");
            setWindowTitle("AGS Setup - " + QDir(gameDir).dirName());
        }
        
        buildUI();
        if (!launcherMode) {
            loadFromConfig();
        }
    }

private:
    QString initialDir;
    QString gameDir;
    QString cfgPath;
    QString sidecarPath;
    bool launcherMode;
    
    // UI Elements
    QComboBox *driver;
    QCheckBox *windowed;
    QComboBox *fullscreenMode;
    QComboBox *scaleFs;
    QCheckBox *vsync;
    QCheckBox *antialias;
    QCheckBox *soundEnabled;
    QCheckBox *speechEnabled;
    QLineEdit *shaderPath;
    QLineEdit *gamePath;
    QTreeView *shaderTree;
    QFileSystemModel *shaderModel;

    void buildUI() {
        QVBoxLayout *root = new QVBoxLayout(this);

        // Game Selection (for launcher mode)
        if (launcherMode) {
            QGroupBox *gameBox = new QGroupBox("Select Game Directory", this);
            QVBoxLayout *gameLayout = new QVBoxLayout(gameBox);
            
            QHBoxLayout *gamePathRow = new QHBoxLayout();
            gamePath = new QLineEdit(this);
            gamePath->setPlaceholderText("Select the game directory containing acsetup.cfg");
            QPushButton *browseGameBtn = new QPushButton("Browse...", this);
            connect(browseGameBtn, &QPushButton::clicked, this, &AGSSetup::browseGameDir);
            gamePathRow->addWidget(gamePath);
            gamePathRow->addWidget(browseGameBtn);
            gameLayout->addLayout(gamePathRow);
            
            QPushButton *openBtn = new QPushButton("Open Setup for Selected Game", this);
            connect(openBtn, &QPushButton::clicked, this, &AGSSetup::openGameSetup);
            gameLayout->addWidget(openBtn);
            
            root->addWidget(gameBox);
            root->addStretch();
            return; // Skip the rest for launcher mode
        }

        // Graphics Group
        QGroupBox *gfxBox = new QGroupBox("Graphics", this);
        QFormLayout *gfxForm = new QFormLayout(gfxBox);
        
        driver = new QComboBox(this);
        driver->addItems(GRAPHICS_DRIVERS);
        windowed = new QCheckBox("Start windowed", this);
        fullscreenMode = new QComboBox(this);
        fullscreenMode->addItems(WINDOW_MODES);
        scaleFs = new QComboBox(this);
        scaleFs->addItems(SCALE_MODES);
        vsync = new QCheckBox("Vsync", this);
        antialias = new QCheckBox("Smooth scaled sprites (antialias)", this);
        
        gfxForm->addRow("Renderer:", driver);
        gfxForm->addRow(windowed);
        gfxForm->addRow("Fullscreen mode:", fullscreenMode);
        gfxForm->addRow("Fullscreen scaling:", scaleFs);
        gfxForm->addRow(vsync);
        gfxForm->addRow(antialias);
        root->addWidget(gfxBox);

        // Shader Group with Tree View
        QGroupBox *shaderBox = new QGroupBox("Shader (librashader - OGL renderer only)", this);
        QVBoxLayout *shaderLayout = new QVBoxLayout(shaderBox);
        
        // Shader path input
        QHBoxLayout *shaderPathRow = new QHBoxLayout();
        shaderPath = new QLineEdit(this);
        shaderPath->setPlaceholderText("None - plain output");
        QPushButton *browseBtn = new QPushButton("Browse...", this);
        QPushButton *clearBtn = new QPushButton("Clear", this);
        QPushButton *shaderBrowserBtn = new QPushButton("Browse Shader Directory...", this);
        
        connect(browseBtn, &QPushButton::clicked, this, &AGSSetup::browseShader);
        connect(clearBtn, &QPushButton::clicked, shaderPath, &QLineEdit::clear);
        connect(shaderBrowserBtn, &QPushButton::clicked, this, &AGSSetup::showShaderBrowser);
        
        shaderPathRow->addWidget(shaderPath);
        shaderPathRow->addWidget(browseBtn);
        shaderPathRow->addWidget(clearBtn);
        shaderLayout->addLayout(shaderPathRow);
        shaderLayout->addWidget(shaderBrowserBtn);
        
        QLabel *note = new QLabel(
            "Requires an engine build with the librashader patch. "
            "Ignored when the renderer above is Software.",
            this
        );
        note->setWordWrap(true);
        shaderLayout->addWidget(note);
        root->addWidget(shaderBox);

        // Sound Group
        QGroupBox *soundBox = new QGroupBox("Sound", this);
        QFormLayout *soundForm = new QFormLayout(soundBox);
        
        soundEnabled = new QCheckBox("Enable sound", this);
        speechEnabled = new QCheckBox("Enable voice speech", this);
        
        soundForm->addRow(soundEnabled);
        soundForm->addRow(speechEnabled);
        root->addWidget(soundBox);

        // Buttons
        QHBoxLayout *btnRow = new QHBoxLayout();
        QPushButton *saveBtn = new QPushButton("Save", this);
        QPushButton *playBtn = new QPushButton("Save && Play", this);
        QPushButton *launchBtn = new QPushButton("Launch Game", this);
        
        connect(saveBtn, &QPushButton::clicked, this, &AGSSetup::save);
        connect(playBtn, &QPushButton::clicked, this, &AGSSetup::saveAndPlay);
        connect(launchBtn, &QPushButton::clicked, this, &AGSSetup::launchGame);
        
        btnRow->addStretch(1);
        btnRow->addWidget(saveBtn);
        btnRow->addWidget(playBtn);
        btnRow->addWidget(launchBtn);
        root->addLayout(btnRow);
    }

    void loadFromConfig() {
        if (launcherMode) return;
        
        QSettings cfg(cfgPath, QSettings::IniFormat);
        
        // Graphics
        setComboIndex(driver, cfg.value("graphics/driver", "OGL").toString());
        windowed->setChecked(cfg.value("graphics/windowed", "0").toString() == "1");
        setComboIndex(fullscreenMode, cfg.value("graphics/fullscreen", "default").toString());
        setComboIndex(scaleFs, cfg.value("graphics/game_scale_fs", "proportional").toString());
        vsync->setChecked(cfg.value("graphics/vsync", "0").toString() == "1");
        antialias->setChecked(cfg.value("graphics/antialias", "0").toString() == "1");
        
        // Sound
        soundEnabled->setChecked(cfg.value("sound/enabled", "1").toString() == "1");
        speechEnabled->setChecked(cfg.value("sound/usespeech", "1").toString() == "1");
        
        // Shader preset (sidecar file)
        QFile sidecar(sidecarPath);
        if (sidecar.exists() && sidecar.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream in(&sidecar);
            shaderPath->setText(in.readAll().trimmed());
            sidecar.close();
        }
    }

    void setComboIndex(QComboBox *combo, const QString &value) {
        int index = combo->findText(value, Qt::MatchFixedString);
        if (index >= 0) {
            combo->setCurrentIndex(index);
        }
    }

    void browseShader() {
        QString path = QFileDialog::getOpenFileName(
            this, "Choose a RetroArch shader preset",
            gameDir, "Shader presets (*.slangp *.glslp);;All files (*)"
        );
        if (!path.isEmpty()) {
            shaderPath->setText(path);
        }
    }

    void showShaderBrowser() {
        QString dir = QFileDialog::getExistingDirectory(
            this, "Select Shader Directory",
            gameDir, QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );
        if (!dir.isEmpty()) {
            // Show file dialog to select a preset from the directory
            QString path = QFileDialog::getOpenFileName(
                this, "Choose a shader preset",
                dir, "Shader presets (*.slangp *.glslp);;All files (*)"
            );
            if (!path.isEmpty()) {
                shaderPath->setText(path);
            }
        }
    }

    void browseGameDir() {
        QString dir = QFileDialog::getExistingDirectory(
            this, "Select Game Directory",
            gamePath->text().isEmpty() ? initialDir : gamePath->text()
        );
        if (!dir.isEmpty()) {
            gamePath->setText(dir);
        }
    }

    void openGameSetup() {
        QString selectedDir = gamePath->text().trimmed();
        if (selectedDir.isEmpty()) {
            QMessageBox::warning(this, "No directory selected", "Please select a game directory first.");
            return;
        }
        
        if (!QFile::exists(QDir(selectedDir).filePath("acsetup.cfg"))) {
            QMessageBox::warning(this, "Invalid directory", 
                "The selected directory does not contain an acsetup.cfg file.\n"
                "Please select a valid AGS game directory.");
            return;
        }
        
        // Close current window and open setup for selected game
        this->close();
        
        // Create new setup window for the selected game
        AGSSetup *setup = new AGSSetup(selectedDir, false);
        setup->resize(420, 420);
        setup->show();
    }

    bool save() {
        if (launcherMode) return false;
        
        QSettings cfg(cfgPath, QSettings::IniFormat);
        
        // Graphics
        cfg.setValue("graphics/driver", driver->currentText());
        cfg.setValue("graphics/windowed", windowed->isChecked() ? "1" : "0");
        cfg.setValue("graphics/fullscreen", fullscreenMode->currentText());
        cfg.setValue("graphics/game_scale_fs", scaleFs->currentText());
        cfg.setValue("graphics/vsync", vsync->isChecked() ? "1" : "0");
        cfg.setValue("graphics/antialias", antialias->isChecked() ? "1" : "0");
        
        // Sound
        cfg.setValue("sound/enabled", soundEnabled->isChecked() ? "1" : "0");
        cfg.setValue("sound/usespeech", speechEnabled->isChecked() ? "1" : "0");
        
        // Write shader preset to sidecar
        QString preset = shaderPath->text().trimmed();
        if (!preset.isEmpty()) {
            QFile sidecar(sidecarPath);
            if (sidecar.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream out(&sidecar);
                out << preset;
                sidecar.close();
            }
        } else {
            QFile::remove(sidecarPath);
        }
        
        if (cfg.status() != QSettings::NoError) {
            QMessageBox::critical(this, "Save failed", "Could not write to acsetup.cfg");
            return false;
        }
        return true;
    }

    QString findGameBinary() {
        QDir dir(gameDir);
        QStringList skip = {"agssetup", "winsetup.exe", "acwin.exe"};
        
        // Prefer executable with same name as directory, then generic "ags"
        QStringList candidates;
        QString dirName = dir.dirName();
        
        for (const QString &name : dir.entryList(QDir::Files | QDir::Executable)) {
            if (skip.contains(name) || 
                name.endsWith(".cfg") || name.endsWith(".ags") ||
                name.endsWith(".so") || name.endsWith(".dll")) {
                continue;
            }
            candidates.append(dir.filePath(name));
        }
        
        // Sort: prefer exact directory name match, then "ags"
        for (const QString &candidate : candidates) {
            QFileInfo info(candidate);
            if (info.fileName() == dirName || info.fileName() == "ags") {
                return candidate;
            }
        }
        
        // Return first candidate if any
        return candidates.isEmpty() ? QString() : candidates.first();
    }

    QString findEngineBinary() {
        // Look for engine binary in common locations
        QStringList paths = {
            QCoreApplication::applicationDirPath(),
            QDir::homePath() + "/.local/share/ags",
            QDir::homePath() + "/ags",
            "/usr/local/bin",
            "/usr/bin"
        };
        
        for (const QString &path : paths) {
            QDir dir(path);
            if (dir.exists("ags") || dir.exists("ags3") || dir.exists("ags-engine")) {
                for (const QString &name : {"ags", "ags3", "ags-engine"}) {
                    QString fullPath = dir.filePath(name);
                    if (QFile::exists(fullPath) && QFileInfo(fullPath).isExecutable()) {
                        return fullPath;
                    }
                }
            }
        }
        return QString();
    }

    void saveAndPlay() {
        if (!save()) {
            return;
        }
        
        QString binary = findGameBinary();
        if (binary.isEmpty()) {
            // Try to find engine binary
            binary = findEngineBinary();
            if (binary.isEmpty()) {
                QMessageBox::warning(
                    this, "Can't find game binary",
                    "No executable found next to acsetup.cfg or in standard locations. "
                    "Launch the game manually - your settings are saved."
                );
                return;
            }
        }
        
        QProcess *proc = new QProcess(this);
        proc->setProgram(binary);
        proc->setWorkingDirectory(gameDir);
        
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString preset = shaderPath->text().trimmed();
        
        if (!preset.isEmpty() && driver->currentText() == "OGL") {
            env.insert("AGS_LIBRASHADER_PRESET", preset);
        } else {
            env.remove("AGS_LIBRASHADER_PRESET");
        }
        
        proc->setProcessEnvironment(env);
        proc->startDetached();
        QApplication::quit();
    }

    void launchGame() {
        QString binary = findGameBinary();
        if (binary.isEmpty()) {
            // Try to find engine binary
            binary = findEngineBinary();
            if (binary.isEmpty()) {
                QMessageBox::warning(
                    this, "Can't find game binary",
                    "No executable found next to acsetup.cfg or in standard locations."
                );
                return;
            }
        }
        
        QProcess *proc = new QProcess(this);
        proc->setProgram(binary);
        proc->setWorkingDirectory(gameDir);
        
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        QString preset = shaderPath->text().trimmed();
        
        if (!preset.isEmpty() && driver->currentText() == "OGL") {
            env.insert("AGS_LIBRASHADER_PRESET", preset);
        } else {
            env.remove("AGS_LIBRASHADER_PRESET");
        }
        
        proc->setProcessEnvironment(env);
        proc->startDetached();
    }
};


int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    QString initialDir;
    bool launcherMode = false;
    
    // Check if launched with --launcher flag
    for (int i = 1; i < argc; i++) {
        if (QString(argv[i]) == "--launcher" || QString(argv[i]) == "-l") {
            launcherMode = true;
        }
    }
    
    if (argc > 1 && !launcherMode) {
        initialDir = QDir(argv[1]).absolutePath();
    } else {
        initialDir = QDir::current().absolutePath();
    }
    
    if (launcherMode) {
        AGSSetup win(initialDir, true);
        win.resize(500, 200);
        win.show();
    } else {
        AGSSetup win(initialDir);
        win.resize(420, 420);
        win.show();
    }
    
    return app.exec();
}

#include "agssetup.moc"
