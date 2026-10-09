# Local verification

The harness needs Python 3, Qt 6 qmake (`qmake6` or Qt 6 `qmake`), make and a C++17
compiler. Install the Qt modules listed in the README. Python uses only its
standard library. A running desktop and a portal backend are unnecessary for
automated checks.

```sh
bin/build
bin/test
bin/harness check
```

`check` first requires matching application, PKGBUILD and README release versions,
then verifies the tools and release safeguards, builds the application, runs QtTest including real
application processes, and validates screenshot/state evidence. It uses Qt's
offscreen platform, software rendering, a device scale of 1 and isolated XDG
directories. Tests supply their own profile directories and document fixtures;
`HOME` remains unchanged. Tests load shipped QML resources and fonts.

Each check has a unique `.harness/runs/<time>-<id>/` directory and prints its
absolute location. Shared qmake output directories are protected by an advisory
build lock, so concurrent harness checks do not race compilation or tests.

## Results and evidence

- `report.json` records tool versions, commands, timings, exit codes, test counts,
  diagnostic failures and paths to evidence.
- `build.log` and `tests.log` preserve stdout and stderr; `results.xml` contains
  machine-readable JUnit-style QtTest results.
- `release-tests.log` records version consistency, archive validation and
  standalone AUR recipe checks. Actual `.SRCINFO` generation is covered when
  Arch's `makepkg` is available; publishing is a separate authorized release step.
- `artifacts/` holds PNGs paired with JSON state snapshots for dark and light
  windows, minimum size with 4/3 text scale, search, unsaved-change decisions and
  external conflicts.
- `profile/tests/` preserves each test's separate settings, document and recovery
  fixture. `runtime/` holds the harness's initial temporary environment; Qt
  profile communication uses its short private runtime directory.

A failed stage returns nonzero and leaves its available evidence in place. QML
tests reject unexpected warnings; the harness additionally checks the text log
for common QML failures and binding loops. Missing or invalid result files and
screenshot/state pairs fail verification. Build time is capped at 10 minutes and
tests at 5 minutes; adjust the latter with `bin/harness check --timeout 600`.
Timed-out check process groups are terminated to prevent leftover app processes.

Review images after interface changes. Their presence proves a capture was
produced, not that the design looks correct. Behavior and geometry assertions
provide automated coverage without fragile pixel comparisons. Do not regenerate
an image just to hide an unexplained warning or failed check.

For a focused QtTest run, pass a test function to `bin/test`. For XML output, the
script also forwards standard QtTest arguments:

```sh
bin/test countsWords
bin/test -o /tmp/omascribe-tests.xml,junitxml -o -,txt
```

Focused checks supplement the complete check; they do not supply all required
harness artifacts.

## Reproduce a problem in isolation

```sh
bin/harness run
bin/harness run --profile /absolute/path/printed/by/the/first/run
bin/harness run --theme light --scale 1.333333333
bin/harness run --headless --profile /tmp/omascribe-control-demo
```

`run` builds if the binary is missing. Use `--build` after changing source when
you want it to rebuild. It prints the profile location and keeps the app in the
foreground. Reusing a profile preserves preferences and recovery, including
drafts left by a crash. The default scale for each run is 1; an explicit
`--theme` replaces that profile's palette. Supply only fixture files: a profile
isolates application state, but opening an external file still allows explicit
operations on that file.

## Covered behavior

Tests cover unsaved-document reads; selection and replacement; UTF-16 and line
endings; undo/redo; stale versions; open/save/reload/close decisions; failed reads
and saves; disk conflicts before watcher delivery; recovery after forced exit;
pre-crash disk baselines and conservative legacy recovery; independent windows;
malformed requests and control transport failures. UI
fixtures exercise the rendered application rather than a copied test-only view.

Use signal/condition waits and rendering completion rather than fixed delays.
Real-process fixtures must close or terminate child applications on every exit,
including assertion failure. All filesystem examples and tests must use
temporary or harness-owned directories.

## Manual checks on a desktop

Offscreen verification cannot establish correct desktop integration. Before
shipping a change affecting it, use fixture documents and a desktop for:

- File open/save portal behavior and canceling a native picker in normal mode.
- Printing, print cancellation and pagination with the system dialog.
- IME composition, non-Latin text entry and keyboard focus after dialogs/search.
- Focus and shortcuts with multiple windows, including Ctrl+N and close.
- Fractional display scaling, moving between monitors and live desktop text-scale
  changes. The 4/3 automated fixture exercises text scale, not monitor compositor
  behavior.

Normal-mode desktop theme following is checked separately from deterministic
profiles. Record any unperformed manual check in the handoff; do not describe
offscreen screenshots as evidence for portals, printing or physical monitor scale.
