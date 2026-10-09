# Local JSON control

Every Omascribe window exposes a same-user local socket. No HTTP service, MCP
server, AI provider or token is required. Control sees the live document,
including unsaved text, and uses the document protections shared with the UI.

## Discover and address a window

```sh
build/omascribe --control list
build/omascribe --profile /absolute/profile --control list
```

Both commands emit one JSON response. A successful list has
`result.sessions`, an array of reachable session metadata. Use its `id` with
`--session`; include the same `--profile` for isolated windows:

```sh
printf '%s\n' '{"protocol":1,"command":"state","args":{}}' |
  build/omascribe --profile /absolute/profile --control --session SESSION_ID
```

`--control` creates no graphical window. JSON is written only to stdout and
diagnostics to stderr. It returns nonzero on failure. Normal-window discovery
does not include isolated profiles. Ctrl+N inherits the original profile but
creates a different session ID.

Exit status 0 means success, 1 a definite request/application error, 2 an unknown
outcome after the request was sent, and 3 an unavailable session or a request
that was not sent. In particular, status 2 must not trigger an automatic retry
of a mutation.

## Requests and responses

The protocol is JSONL version 1: one UTF-8 JSON object followed by a newline,
one request and one response per connection. The maximum message size is
16 MiB and the timeout is 5 seconds. Commands are serialized on the UI thread.

```json
{"protocol":1,"command":"state","args":{"includeText":true}}
```

Successful responses have `protocol`, `ok: true` and `result`. Errors have
`protocol`, `ok: false`, `error: {code, message}` and, when a live session was
reached, current `state` metadata. Mutation results contain metadata and the
new `version` rather than another copy of all the text. Treat `version` as an
opaque string; it belongs only to the current session.

`state` includes `id`, `pid`, `version`, `file` (absolute path, empty for an
unnamed draft), `modified`, `canUndo`, `canRedo`, `cursor`, `selectionStart`,
`selectionEnd`, `externalChanged`, `externalDeleted`, `pendingAction`,
`activeDialog`, `darkMode`, `textScale`, and `window` metadata including width,
height, device pixel ratio and visibility. Text is included by default. Use
`includeText: false` for metadata only. A document that would exceed the response
limit produces `response_too_large` with metadata instead of truncated text.

| Command | Arguments in `args` |
| --- | --- |
| `state` | Optional `includeText` (default `true`). |
| `select` | `expectedVersion`, `start`, `end`. |
| `replace_range` | `expectedVersion`, `start`, `end`, `text`. |
| `undo`, `redo` | `expectedVersion`. |
| `open` | `expectedVersion`, absolute `path`; optional dirty-document decision below. |
| `save` | `expectedVersion`; saves the current named file. |
| `save_as` | `expectedVersion`, absolute `path`, optional `overwrite` (default `false`). |
| `reload`, `close` | `expectedVersion`; optional dirty-document decision below. |
| `resolve_external` | `expectedVersion`, `choice: "keep"` or `"reload"`; a reload of dirty text needs a decision. |
| `screenshot` | Absolute `path`, optional `overwrite` (default `false`) and `expectedVersion`. |

All document/window mutations require `expectedVersion`. A stale version returns
`version_conflict` without executing the requested mutation. Read the current
state and decide again; do not automatically overwrite a user's intervening edit.
Opening another file with identical text and restarting the app both invalidate
old versions.

Text offsets are zero-based UTF-16 code units, with an end-exclusive range.
For example, `A😀B` has length 4; the emoji occupies `[1,3)`. An interval that
splits it is rejected. Replace a range with `text: ""` to delete, or use
`start == end` to insert. CRLF and CR are normalized to LF. Each replacement is
one undo step and places the caret at the end of the inserted text.

## Unsaved text and conflicts

Dirty `open`, `reload` and `close` return `unsaved_changes` without a decision.
Add `onUnsaved: "discard"` to explicitly discard, or `onUnsaved: "save"` to
save before continuing. For an unnamed draft, the latter also requires an
absolute `savePath`; use `overwrite: true` if that save targets another existing
file. For a standalone unnamed save, use `save_as`; `save` returns
`save_path_required`.

The destination is read before dirty text is discarded during open/reload. If
reading or saving fails, the draft remains. A failed save cancels the pending
transition. A compound operation can save successfully and then fail later, so
inspect the error's state to understand the actual outcome.

Saving checks the current file on disk even before a watcher event. A changed
file returns `disk_conflict`. Resolve it using `resolve_external` with
`choice: "keep"` to retain the local draft and allow its next save, or
`choice: "reload"` to load disk contents. Dirty reload requires an explicit
save/discard choice. After an external deletion, Keep Mine retains the path and
the next save recreates it. Equivalent paths and symbolic links to the same
file still get these protections.

Reusing a profile after a crash recovers unsaved drafts. A named recovered draft
retains its pre-crash disk baseline, so a file edited while the app was absent
requires conflict resolution before saving (`busy` while its dialog is open,
or `disk_conflict` when detected directly by a save). Older recovery records without a
valid baseline conservatively require resolution before overwriting an existing
named file. Inspect the recovered text and choose Keep Mine or Reload explicitly.

During a pending UI decision, mutations return `busy`. State and screenshots
remain available, as does the relevant external-conflict resolution. Successful
control close responds before the window exits. After a transport timeout or
disconnection, a mutation may already have happened: re-read state or rediscover
sessions rather than retrying the mutation automatically.

Other error codes include `invalid_request`, `invalid_range`, `io_error`,
`target_exists`, `unknown_command` and `capture_unavailable`. The transport also
reports `invalid_json`, `unsupported_protocol`, `request_too_large`, `timeout`
and `unavailable`. File and screenshot paths must be absolute.
A screenshot returns its path, image width/height, device pixel ratio and captured
session metadata. It verifies the version after rendering and rejects unavailable
captures or an existing destination without overwrite permission.

## Executable end-to-end example

Run this from the repository after `bin/build`. It creates an isolated headless
window, reads the draft, inserts text, undoes/redoes, saves, captures and closes.
The profile, file, log and screenshot remain under `.harness/examples/` for review.

```sh
python3 - <<'PY'
import json
import os
from pathlib import Path
import subprocess
import time
import uuid

binary = str(Path("build/omascribe").resolve())
directory = Path(".harness/examples").resolve() / uuid.uuid4().hex
directory.mkdir(parents=True)
profile = str(directory / "profile")
base = [binary, "--profile", profile, "--control"]
env = dict(os.environ, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software")
env.update(OMASCRIBE_PROFILE_THEME="dark", OMASCRIBE_PROFILE_SCALE="1")
log = (directory / "app.log").open("w")
app = subprocess.Popen([binary, "--profile", profile], env=env,
                       stdout=log, stderr=log)
try:
    deadline = time.monotonic() + 5
    sessions = []
    while not sessions and time.monotonic() < deadline:
        reply = subprocess.run(base + ["list"], capture_output=True, text=True)
        sessions = json.loads(reply.stdout).get("result", {}).get("sessions", [])
        if not sessions:
            if app.poll() is not None:
                raise RuntimeError("Application exited; inspect app.log")
            time.sleep(0.05)
    if not sessions:
        raise RuntimeError("No session discovered")
    session = sessions[0]["id"]

    def request(command, **args):
        message = {"protocol": 1, "command": command, "args": args}
        process = subprocess.run(base + ["--session", session],
                                 input=json.dumps(message) + "\n",
                                 capture_output=True, text=True, timeout=6)
        response = json.loads(process.stdout)
        if not response["ok"]:
            raise RuntimeError(response)
        return response["result"]

    state = request("state")
    state = request("replace_range", expectedVersion=state["version"],
                    start=0, end=0, text="# Harness draft\nHello from an agent.\n")
    state = request("undo", expectedVersion=state["version"])
    state = request("redo", expectedVersion=state["version"])
    state = request("save_as", expectedVersion=state["version"],
                    path=str(directory / "draft.md"))
    request("screenshot", expectedVersion=state["version"],
            path=str(directory / "window.png"))
    (directory / "window.json").write_text(json.dumps(request("state"), indent=2))
    request("close", expectedVersion=state["version"])
    app.wait(timeout=5)
    print(directory)
finally:
    if app.poll() is None:
        app.kill()
        app.wait()
    log.close()
PY
```

For an interactive experiment use `bin/harness run`; the same discovery and
command interface controls that visible window. Never point a test at a personal
document: profile isolation does not prevent an explicit save to a supplied path.
