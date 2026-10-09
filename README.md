# Omascribe

A Markdown writing app built with Qt Quick and C++ that automatically follows system dark/light mode.

Omascribe is based on [Omawrite](https://github.com/omacom-io/omawrite) and continues
its version sequence with **0.7.0**. This fork is maintained at
[RolandDumit/omascribe](https://github.com/RolandDumit/omascribe).

<img width="2948" height="3227" alt="screenshot-2026-06-23_15-24-08" src="https://github.com/user-attachments/assets/4e930c0d-edda-4046-b444-a59eff523329" />
<img width="2948" height="3227" alt="screenshot-2026-06-23_15-23-23" src="https://github.com/user-attachments/assets/8ced7c26-961b-4ded-b263-84403001a951" />


## Install

Install the current checkout as an Arch package from a terminal:

```sh
bin/install
```

This builds and installs `omascribe` 0.7.0-1 through makepkg and pacman, including
the application launcher and icon. Run `omascribe` afterwards. It can coexist
with the official `omawrite` package: binaries, desktop entries, settings and
recovery data use separate names. Existing Omawrite preferences and drafts are
not automatically imported.

While developing, rerun `bin/install` after changes; use `bin/harness run` for
isolated experiments. The local package does not follow GitHub pushes
automatically. Every GitHub release also requires updating the PKGBUILD and
publishing its matching AUR recipe. See [packaging and release updates](docs/packaging.md)
for the mandatory workflow and `bin/harness prepare-aur` helper. Omascribe is not distributed through the
official Omarchy package repository.

## Shortcuts

- `Ctrl+S` saves. Unsaved documents use the XDG desktop portal file picker.
- `Ctrl+Shift+S` saves as.
- `Ctrl+O` opens a Markdown file through the portal picker.
- `Ctrl+P` opens the system print dialog.
- `Ctrl+N` opens a new Omascribe window.
- `Ctrl+Z`, `Ctrl+Shift+Z`, and `Ctrl+Y` handle undo and redo.
- `Super+F` toggles fullscreen. Qt maps this key as `Meta+F`.
- `Ctrl+F` searches the document. Use `Enter` or `Ctrl+G` for the next match and `Shift+Enter` for the previous match.
- `Ctrl+H` opens find and replace.
- `Ctrl+B`, `Ctrl+I`, and `Ctrl+K` insert bold, italic, and link Markdown.
- `Ctrl+?` shows the keyboard shortcut reference.

Unsaved drafts are recovered after an abnormal exit. Omascribe also watches open files
and warns before an external change can replace local work.

The font button in the footer picks the writing font from any installed text font,
and Omascribe remembers the choice. IBM Plex Mono is the default.

Text follows the desktop text size — `omarchy display text size`, or GNOME's
`text-scaling-factor` — and re-flows without a restart. The default of 12px leaves
Omascribe at the size it is designed around; larger and smaller sizes scale from there.

## Requirements

- Qt 6: `qt6-base`, `qt6-declarative`, `qt6-quickcontrols2`
- `xdg-desktop-portal` and a portal backend

The IBM Plex Mono font is bundled under the SIL Open Font License 1.1; see
`fonts/OFL.txt`. The font is copyright IBM Corp.

## Development and local agent control

Build with `bin/build`, run focused tests with `bin/test`, and run the complete
isolated verification with `bin/harness check`. The harness needs Python 3, Qt 6
qmake, make and a C++17 compiler. It saves logs, QtTest results, JSON state and
screenshots under `.harness/runs/` and returns nonzero on a failed check.

`bin/harness run [file]` starts a separate profile for experiments. It prints the
profile directory; reuse it with `--profile /absolute/path` to reproduce settings
or recovery after a crash. The profile defaults to a deterministic dark theme
and text scale 1. `--theme light`, `--scale 1.333333333` and `--headless` are
available for interface and control checks.

Every window exposes same-user local JSON control. `build/omascribe --control
list` discovers normal windows; add `--profile /absolute/path` to discover an
isolated profile. `--control --session <id>` reads one request from stdin. Agents
can inspect a draft, edit with undo, save and capture the actual window, using
version checks and the same document protections as the UI.

See [architecture](docs/architecture.md), [testing](docs/testing.md) and
[agent control with executable examples](docs/agent-control.md). Desktop portals,
printing, IME and physical display scaling still need desktop verification.
