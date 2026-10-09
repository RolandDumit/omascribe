# Architecture and document protections

Omascribe is a single-window Qt Quick editor per application process. Ctrl+N starts
another process; each window has its own live document, undo history and control
session. The application uses Qt 6, qmake and C++17, with no AI service or network
dependency in the harness.

## Responsibilities

- **QML** renders the editor, search, footer and dialogs. Stable object names let
  tests inspect the actual shipped interface. Markdown text is held by the
  editor's `QTextDocument`; the highlighter changes presentation, not source text.
- **Backend** handles file I/O, file watching, settings, recovery, fonts and
  Markdown presentation. Changes to the live document are reflected in the UI,
  recovery and the local control interface.
- **EditorSession** coordinates open, save, reload and close transitions for both
  the UI and agent commands. It owns session versioning and the decisions that
  protect unsaved work. A control command cannot bypass a protection implemented
  for a user action.
- **Local control** receives one JSON request per local connection and dispatches
  it on the UI thread. Each process has a random session identity. The command-line
  client uses core Qt facilities without initializing a graphical application.
- **Shared initialization** loads the bundled IBM Plex Mono fonts and Material
  style for both the application and QML tests. Isolated profiles choose a
  deterministic palette and text scale before Qt starts.

## Document views

Source, Split view and Preview share one persistent `sourceEditor` and its
`QTextDocument`. The read-only preview has a separate Markdown document bound to
the live source; it never attaches to Backend or participates in document
transitions. Mode changes do not invalidate session versions or change recovery,
modified state, selection or undo history. Control edits still target the source
when it is hidden, and update the preview normally.

Both panes use `DocumentViewport` for wheel scrolling. In Split view their
scrollable extents are synchronized proportionally, with a reentrancy guard and
deferred alignment after layout changes. This is not a mapping between source
lines and rendered blocks. The draggable split respects minimum pane widths;
resizing the window preserves the preferred ratio. `editor/viewMode` and
`editor/splitRatio` use the existing profile-aware QSettings configuration,
defaulting to source and 0.5 when missing or invalid.

Preview shares the source font, theme and scale. Relative resources use the named
file's directory; untitled resource lookup stays in the application's data
directory. Find and Replace reveal the source in Split view. UI formatting and
undo/redo shortcuts require source focus. Save, open and print still use
EditorSession in every view.

Preview formatting applies font-relative margins to its separate QTextDocument
after Markdown parsing and font changes. Ordinary block gaps are 1 em, heading
top margins 1.5 em, and adjacent list-item gaps 0.25 em. Code lines and table
cells do not receive paragraph margins; tables use frame margins, with empty
structural paragraphs collapsed. Markdown hard breaks remain inside their
paragraph: a comparison parse with soft breaks identifies the QTextBlocks that
Qt splits for those breaks. Content line height, source formatting, undo history
and session versions are unaffected. Explicit
margins avoid Qt's Markdown paragraph defaults relying on point-sized fonts
while Omascribe uses pixel-sized fonts.

## Profiles and discovery

Normal launches keep the existing desktop settings and theme behavior. A launch
with `--profile /absolute/directory` uses that directory for configuration, data,
recovery, cache, documents and theme. It does not replace `HOME`, read the desktop
theme or ask a native file picker. New windows inherit the profile. A new profile
defaults to dark mode and text scale 1.

`OMASCRIBE_PROFILE_THEME=dark|light` explicitly sets a deterministic profile palette;
without the variable an existing profile palette is preserved.
`OMASCRIBE_PROFILE_SCALE` sets a positive text scale. These variables apply only to
profile launches. The profile contains no agent credentials.

For Unix socket path-length limits, communication uses a short private directory
under `/tmp/omascribe-<uid>/`, keyed by the profile's absolute path. It is
separate from normal-window discovery and other profiles even when profile paths
are long. Discovery is a view of live sessions; a previous process's session ID
and version cannot authorize a new process. Directories and sockets are restricted
to the same operating-system user.

## Transition invariants

The session reads an open/reload destination successfully before replacing dirty
text. A failed read leaves the existing text, path and recovery intact. Dirty
open/reload/close operations require a save or discard decision; the UI obtains
it through its dialogs and control commands provide it explicitly.

A failed save cancels a pending transition. A later successful save must not
accidentally close the window or resume an earlier open. When a save succeeds but
a later part of a compound operation fails, the response reports the actual
current state.

Before saving the current file, the session compares its contents on disk with
the known contents, even if no watcher notification has arrived. External changes
require an explicit Keep Mine or Reload decision. Saving through an equivalent
path or symbolic link does not bypass that check. A save to a different existing
file requires explicit overwrite permission through control.

Discard and successful save clear the relevant recovery data. An abnormal exit
leaves it for a subsequent process using the same profile. Each window keeps an
independent recovery record. Keep Mine after an external deletion keeps the path,
so a subsequent save can recreate the file.

A named recovery snapshot preserves the disk contents last acknowledged before
the crash. Recovery does not accept edits made on disk while Omascribe was absent;
the next save detects the difference and requires Keep Mine or Reload. Legacy
snapshots without a valid disk baseline also require explicit resolution before
overwriting an existing named file. Unnamed recovered drafts can be saved to an
explicit new path normally.

## Versioning and editing

A version combines the live session identity with a revision. Text changes,
undo/redo, document replacement, path changes and conflict decisions invalidate
previous versions. All control mutations compare the expected version before
acting; an old version cannot accidentally edit another file with identical text.

Offsets use Qt's UTF-16 code units and end-exclusive intervals. Splitting a
surrogate pair is invalid. Inserted line endings are normalized to LF and a
replacement is one undo step, leaving the caret at the end of the insertion.
The existing product behavior is retained: undo back to saved contents may leave
the modified indicator set.

Screenshot requests observe the actual `QQuickWindow`. They wait for rendering
and verify the version again before reporting the captured state. The harness
records state alongside images; images are review evidence rather than exact
pixel baselines.
