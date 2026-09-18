#!/usr/bin/env python3
"""
agssetup.py — a native-Linux replacement for AGS's winsetup.exe.

winsetup.exe is a small Windows dialog that ships with every AGS game to
edit acsetup.cfg (resolution, renderer, sound, ...) before launching. It
doesn't run on Linux, and it has no concept of shader presets. This script
does the same job for the librashader-enabled engine build: it reads and
writes the game's acsetup.cfg with Python's stdlib configparser (no custom
INI parsing), and adds one thing winsetup never had — a picker for a
librashader .slangp/.glslp preset, passed to the patched engine via the
AGS_LIBRASHADER_PRESET environment variable it already reads (see
librashader-integration.patch).

Usage:
    python3 agssetup.py /path/to/game/folder

If no path is given, the current directory is used, matching where you'd
normally double-click winsetup.exe from.
"""
import configparser
import os
import subprocess
import sys

from PySide6.QtWidgets import (
    QApplication, QWidget, QFormLayout, QVBoxLayout, QHBoxLayout,
    QComboBox, QCheckBox, QSpinBox, QLineEdit, QPushButton, QFileDialog,
    QLabel, QMessageBox, QGroupBox,
)

# Keys this tool touches, exactly as read by Engine/main/config.cpp.
# Anything else already present in acsetup.cfg is preserved untouched.
GRAPHICS_DRIVERS = ["OGL", "Software"]  # D3D9 is Windows/Wine-only, left out here
WINDOW_MODES = ["default", "fullscreen", "fullscreen_borderless", "fullscreen_desktop", "windowed"]
SCALE_MODES = ["max_round", "stretch", "proportional", "round"]


class AGSSetup(QWidget):
    def __init__(self, game_dir: str):
        super().__init__()
        self.game_dir = game_dir
        self.cfg_path = os.path.join(game_dir, "acsetup.cfg")
        self.cfg = configparser.ConfigParser()
        self.cfg.optionxform = str  # preserve key case as AGS wrote it
        if os.path.exists(self.cfg_path):
            self.cfg.read(self.cfg_path)

        self.setWindowTitle(f"AGS Setup — {os.path.basename(game_dir) or game_dir}")
        self._build_ui()
        self._load_from_cfg()

    # ---- UI ---------------------------------------------------------------
    def _build_ui(self):
        root = QVBoxLayout(self)

        gfx_box = QGroupBox("Graphics")
        gfx_form = QFormLayout(gfx_box)
        self.driver = QComboBox(); self.driver.addItems(GRAPHICS_DRIVERS)
        self.windowed = QCheckBox("Start windowed")
        self.fullscreen_mode = QComboBox(); self.fullscreen_mode.addItems(WINDOW_MODES)
        self.vsync = QCheckBox("Vsync")
        self.scale_fs = QComboBox(); self.scale_fs.addItems(SCALE_MODES)
        self.antialias = QCheckBox("Smooth scaled sprites (antialias)")
        gfx_form.addRow("Renderer:", self.driver)
        gfx_form.addRow(self.windowed)
        gfx_form.addRow("Fullscreen mode:", self.fullscreen_mode)
        gfx_form.addRow("Fullscreen scaling:", self.scale_fs)
        gfx_form.addRow(self.vsync)
        gfx_form.addRow(self.antialias)
        root.addWidget(gfx_box)

        shader_box = QGroupBox("Shader (librashader — OGL renderer only)")
        shader_form = QFormLayout(shader_box)
        row = QHBoxLayout()
        self.shader_path = QLineEdit()
        self.shader_path.setPlaceholderText("None — plain output")
        browse = QPushButton("Browse…")
        browse.clicked.connect(self._browse_shader)
        clear = QPushButton("Clear")
        clear.clicked.connect(lambda: self.shader_path.setText(""))
        row.addWidget(self.shader_path); row.addWidget(browse); row.addWidget(clear)
        shader_form.addRow("Preset (.slangp/.glslp):", row)
        note = QLabel("Requires an engine build with the librashader patch. "
                       "Ignored when the renderer above is Software.")
        note.setWordWrap(True)
        shader_form.addRow(note)
        root.addWidget(shader_box)

        sound_box = QGroupBox("Sound")
        sound_form = QFormLayout(sound_box)
        self.sound_enabled = QCheckBox("Enable sound")
        self.speech_enabled = QCheckBox("Enable voice speech")
        sound_form.addRow(self.sound_enabled)
        sound_form.addRow(self.speech_enabled)
        root.addWidget(sound_box)

        btn_row = QHBoxLayout()
        save_btn = QPushButton("Save")
        save_btn.clicked.connect(self._save)
        play_btn = QPushButton("Save && Play")
        play_btn.clicked.connect(self._save_and_play)
        btn_row.addStretch(1)
        btn_row.addWidget(save_btn)
        btn_row.addWidget(play_btn)
        root.addLayout(btn_row)

    # ---- config <-> UI ------------------------------------------------
    def _load_from_cfg(self):
        g = self.cfg["graphics"] if self.cfg.has_section("graphics") else {}
        s = self.cfg["sound"] if self.cfg.has_section("sound") else {}

        self._set_combo(self.driver, g.get("driver", "OGL"))
        self.windowed.setChecked(g.get("windowed", "0") in ("1", "true", "True"))
        self._set_combo(self.fullscreen_mode, g.get("fullscreen", "default"))
        self._set_combo(self.scale_fs, g.get("game_scale_fs", "proportional"))
        self.vsync.setChecked(g.get("vsync", "0") in ("1", "true", "True"))
        self.antialias.setChecked(g.get("antialias", "0") in ("1", "true", "True"))

        self.sound_enabled.setChecked(s.get("enabled", "1") in ("1", "true", "True"))
        self.speech_enabled.setChecked(s.get("usespeech", "1") in ("1", "true", "True"))

        # The shader preset is not an AGS config key at all — it lives
        # only in this tool, remembered in a tiny sidecar file so it
        # survives between runs without touching acsetup.cfg's format.
        sidecar = self._shader_sidecar_path()
        if os.path.exists(sidecar):
            with open(sidecar) as f:
                self.shader_path.setText(f.read().strip())

    @staticmethod
    def _set_combo(combo: QComboBox, value: str):
        for j in range(combo.count()):
            if combo.itemText(j).lower() == str(value).lower():
                combo.setCurrentIndex(j)
                return
        combo.setCurrentIndex(0)

    def _shader_sidecar_path(self) -> str:
        return os.path.join(self.game_dir, ".agssetup_shader_preset")

    def _browse_shader(self):
        path, _ = QFileDialog.getOpenFileName(
            self, "Choose a RetroArch shader preset", self.game_dir,
            "Shader presets (*.slangp *.glslp);;All files (*)")
        if path:
            self.shader_path.setText(path)

    # ---- actions --------------------------------------------------------
    def _apply_to_cfg(self):
        if not self.cfg.has_section("graphics"):
            self.cfg.add_section("graphics")
        if not self.cfg.has_section("sound"):
            self.cfg.add_section("sound")

        g = self.cfg["graphics"]
        g["driver"] = self.driver.currentText()
        g["windowed"] = "1" if self.windowed.isChecked() else "0"
        g["fullscreen"] = self.fullscreen_mode.currentText()
        g["game_scale_fs"] = self.scale_fs.currentText()
        g["vsync"] = "1" if self.vsync.isChecked() else "0"
        g["antialias"] = "1" if self.antialias.isChecked() else "0"

        s = self.cfg["sound"]
        s["enabled"] = "1" if self.sound_enabled.isChecked() else "0"
        s["usespeech"] = "1" if self.speech_enabled.isChecked() else "0"

    def _save(self) -> bool:
        self._apply_to_cfg()
        try:
            # Ensure directory exists
            cfg_dir = os.path.dirname(self.cfg_path)
            if cfg_dir and not os.path.exists(cfg_dir):
                os.makedirs(cfg_dir, exist_ok=True)
            
            with open(self.cfg_path, "w") as f:
                self.cfg.write(f)
            
            preset = self.shader_path.text().strip()
            sidecar = self._shader_sidecar_path()
            if preset:
                # Validate preset path exists
                if not os.path.isfile(preset):
                    QMessageBox.warning(
                        self, "Invalid preset path",
                        f"Shader preset file does not exist: {preset}"
                    )
                    return False
                # Store relative path if possible
                if os.path.isabs(preset):
                    try:
                        rel_preset = os.path.relpath(preset, self.game_dir)
                        preset = rel_preset
                    except (ValueError, OSError):
                        pass  # Keep absolute if relpath fails
                
                with open(sidecar, "w") as f:
                    f.write(preset)
            elif os.path.exists(sidecar):
                os.remove(sidecar)
        except OSError as e:
            QMessageBox.critical(self, "Save failed", str(e))
            return False
        return True

    def _find_game_binary(self):
        # AGS games ship either a binary named after the game, or the
        # generic "ags" launcher plus a .ags data file. Good enough for a
        # setup dialog: prefer an executable that isn't this script's own
        # dependencies and isn't a known non-game file.
        skip = {"agssetup.py", "winsetup.exe", "acwin.exe"}
        candidates = []
        dir_name = os.path.basename(self.game_dir.rstrip('/\\'))
        
        for name in sorted(os.listdir(self.game_dir)):
            full = os.path.join(self.game_dir, name)
            if name in skip or not os.path.isfile(full):
                continue
            if os.access(full, os.X_OK) and not name.endswith((".cfg", ".ags", ".so", ".dll")):
                # Prefer exact directory name match, then "ags"
                if name == dir_name or name == "ags":
                    return full
                candidates.append(full)
        return candidates[0] if candidates else None

    def _save_and_play(self):
        if not self._save():
            return
        binary = self._find_game_binary()
        if not binary:
            QMessageBox.warning(self, "Can't find game binary",
                                 "No executable found next to acsetup.cfg. "
                                 "Launch the game manually — your settings are saved.")
            return
        env = os.environ.copy()
        preset = self.shader_path.text().strip()
        if preset and self.driver.currentText() == "OGL":
            env["AGS_LIBRASHADER_PRESET"] = preset
        elif "AGS_LIBRASHADER_PRESET" in env:
            del env["AGS_LIBRASHADER_PRESET"]
        subprocess.Popen([binary], cwd=self.game_dir, env=env)
        self.close()


def main():
    game_dir = sys.argv[1] if len(sys.argv) > 1 else os.getcwd()
    app = QApplication(sys.argv)
    win = AGSSetup(os.path.abspath(game_dir))
    win.resize(420, 420)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
