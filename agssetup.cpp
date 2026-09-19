// agssetup.cpp - Native Linux replacement for AGS's winsetup.exe
// Uses Qt6 (no Python required). Compile with:
//   qmake6 -project && qmake6 && make
// or with CMake (see CMakeLists.txt)
//
// Reads/writes acsetup.cfg and allows selecting a librashader preset.
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
    AGSSetup(const QString &gameDir, QWidget *parent = nullptr)
        : QWidget(parent), gameDir(gameDir) {
        cfgPath = QDir(gameDir).filePath("acsetup.cfg");
        sidecarPath = QDir(gameDir).filePath(".agssetup_shader_preset");
        
        setWindowTitle("AGS Setup - " + QDir(gameDir).dirName());
        buildUI();
        loadFromConfig();
    }

private:
    QString gameDir;
    QString cfgPath;
    QString sidecarPath;
    
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

    void buildUI() {
        QVBoxLayout *root = new QVBoxLayout(this);

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

        // Shader Group
        QGroupBox *shaderBox = new QGroupBox("Shader (librashader - OGL renderer only)", this);
        QFormLayout *shaderForm = new QFormLayout(shaderBox);
        
        QHBoxLayout *shaderRow = new QHBoxLayout();
        shaderPath = new QLineEdit(this);
        shaderPath->setPlaceholderText("None - plain output");
        QPushButton *browseBtn = new QPushButton("Browse...", this);
        QPushButton *clearBtn = new QPushButton("Clear", this);
        
        connect(browseBtn, &QPushButton::clicked, this, &AGSSetup::browseShader);
        connect(clearBtn, &QPushButton::clicked, shaderPath, &QLineEdit::clear);
        
        shaderRow->addWidget(shaderPath);
        shaderRow->addWidget(browseBtn);
        shaderRow->addWidget(clearBtn);
        shaderForm->addRow("Preset (.slangp/.glslp):", shaderRow);
        
        QLabel *note = new QLabel(
            "Requires an engine build with the librashader patch. "
            "Ignored when the renderer above is Software.",
            this
        );
        note->setWordWrap(true);
        shaderForm->addRow(note);
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
        
        connect(saveBtn, &QPushButton::clicked, this, &AGSSetup::save);
        connect(playBtn, &QPushButton::clicked, this, &AGSSetup::saveAndPlay);
        
        btnRow->addStretch(1);
        btnRow->addWidget(saveBtn);
        btnRow->addWidget(playBtn);
        root->addLayout(btnRow);
    }

    void loadFromConfig() {
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

    bool save() {
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

    void saveAndPlay() {
        if (!save()) {
            return;
        }
        
        QString binary = findGameBinary();
        if (binary.isEmpty()) {
            QMessageBox::warning(
                this, "Can't find game binary",
                "No executable found next to acsetup.cfg. "
                "Launch the game manually - your settings are saved."
            );
            return;
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
};


int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    
    QString gameDir;
    if (argc > 1) {
        gameDir = QDir(argv[1]).absolutePath();
    } else {
        gameDir = QDir::current().absolutePath();
    }
    
    AGSSetup win(gameDir);
    win.resize(420, 420);
    win.show();
    
    return app.exec();
}

#include "agssetup.moc"
