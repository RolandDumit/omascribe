# Working on Omascribe

Omascribe is a local Markdown editor using Qt 6, Qt Quick, qmake and C++17.

## Find the behavior

- `src/Main.qml` contains the window, editor, search and dialogs. `EditorMutations.js` implements editor text transformations.
- The backend owns file I/O, recovery, settings, watching files and Markdown formatting. `EditorSession` coordinates document transitions for both QML and local agent commands.
- Application initialization and the local control transport live alongside the backend. `tests/tst_omascribe.cpp` exercises backend, QML and real application processes.
- Read `docs/architecture.md`, `docs/testing.md` and `docs/agent-control.md` when working on their corresponding behavior.
- Read `docs/packaging.md` for local installation and release packaging. Keep package and application versions consistent.

## Build and verify

- Run `bin/build` to compile, `bin/test` for focused QtTest runs, and `bin/harness check` for the complete local verification with logs, state and screenshots.
- Use `bin/harness run [file]` for an isolated interactive instance; reuse it with `--profile /absolute/path`. Never use a personal document or the user's settings as a test fixture.
- Inspect the report and artifacts printed by `check`. For QML changes, inspect the relevant screenshots as well as test results. Unexpected QML warnings are failures.
- Keep tests focused on user behavior and failure cases. Add a regression test for a changed document protection or control command; avoid tests that merely repeat implementation details.

## Document rules

- Route open/save/reload/close transitions through `EditorSession`. UI and control commands must share document protections.
- Never discard dirty text before successfully reading the replacement. Failed saves cancel pending transitions.
- Check disk contents before overwriting the current file; explicit conflict resolution is required. Preserve recovery until a successful save or explicit discard.
- Control mutations require the current session version. Text offsets are UTF-16, end-exclusive; do not split surrogate pairs. Replacements must remain one undo step.
- Preserve the existing behavior in which undo back to saved text can still leave the document marked modified.
- Keep normal desktop behavior intact. Isolated profiles must not read or write personal configuration, theme or recovery data.

## Completion

Run the appropriate checks and finish with `bin/harness check` for changes to document behavior, QML, initialization or control. Report what changed, verification performed, artifact location and any manual desktop checks still needed. Do not add a cloud service, MCP server, scheduled automation or AI provider integration unless requested.

## Releases

Every Omascribe version change and GitHub release must update `src/appversion.h`,
`pkgbuild/PKGBUILD` and the README together. Reset `pkgrel` to 1 for a new version;
increment it for packaging-only fixes. `bin/harness check` rejects version drift.

Publishing a release includes publishing the matching `omascribe` package on AUR.
Follow `docs/packaging.md`: verify locally, commit and push, publish the immutable
`v<version>` tag and GitHub release, download its source archive, run
`bin/harness prepare-aur`, verify the standalone package, then commit/push its
PKGBUILD, `.SRCINFO` and install script to AUR. Do not commit source archives,
binaries, credentials or harness evidence to either source repository.

Only report release completion after both GitHub and AUR are published and
verified. If AUR credentials or access are missing, preserve the prepared files,
report the exact missing step and continue all independent authorized work.

AUR publication is currently deferred: on 2026-10-09 the maintainer reported
that new account registrations are closed and confirmed they have no AUR account.
Until access is available, continue GitHub releases and prepare/verify their AUR
recipes, explicitly recording publication as pending. Do not repeatedly request
unavailable credentials or block GitHub releases on this known prerequisite.
Resume AUR publication when the maintainer confirms account and SSH access.
