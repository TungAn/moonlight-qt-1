# Local client integration

Pyrolight remains the authority for client preferences, profiles, trust and
stream lifecycle. Integrations such as PyroDeck use a versioned local contract
and the existing paired GameStream connection. No remote control listener is
added.

## Preferences and profiles

`pyrolight settings` prints a schema-1 JSON snapshot containing `revision`,
`activeProfileId`, `activeProfileName`, `profiles`, `values`, `registry` and
`capabilities`. Registry descriptors identify value type, numeric bounds,
enum choices, global/profile scope, platform support, dependencies and runtime
applicability. Every current preference is classified as requiring reconnect;
persisting a preference never claims to change a running stream.

Mutations use `pyrolight settings --json REQUEST`, with an argv argument rather
than shell interpolation. For example, substitute a revision just read:

```json
{"schema":1,"operation":"patch","revision":"REVISION","values":{"fps":90,"audioQuality":2}}
```

| Operation | Additional fields | Behavior |
| --- | --- | --- |
| `read` | None | Current authoritative snapshot, including unsaved GUI edits |
| `defaults` | None | Return profile defaults for staging without modifying the current preferences |
| `validate` | `revision`, `values` | Check complete patch without mutation |
| `patch` | `revision`, `values` | Validate all values before changing/saving any |
| `create` | `revision`, `name` | Create and select a profile through the QML profile implementation |
| `duplicate` | `revision`, `profileId`, `name` | Duplicate and select |
| `rename` | `revision`, `profileId`, `name` | Rename; Default protected |
| `delete` | `revision`, `profileId` | Delete; deleting active selects Default; Default protected |
| `activate` | `revision`, `profileId` | Persist one active profile |
| `restore` | `revision` | Restore active profile defaults; globals preserved |
| `prepare-launch` | None | Persist current GUI edits and return the exact next-launch snapshot |

Success returns `schema:1, ok:true`. Failures return `ok:false` and
`error:{code,message}` with a nonzero CLI exit status. Revision conflicts do
not partially write. Bitrate calculation, profile normalization and VRR presets
stay in StreamingPreferences. No external process should edit Qt settings files.

A standalone GUI owns the private same-user `QLocalServer` and a lock keyed by
its settings path; CLI transactions are marshalled onto that GUI's thread.
With no GUI, the CLI takes the same lock and runs without initializing a
display. Portable settings paths have distinct endpoints. Concurrent GUI owners
are refused; stale CLI transactions are rejected. External writes are refused
while that standalone GUI is streaming, because it shares its preference object
with its Session. Separate CLI stream processes retain their launch snapshot.

## Stable launch targets and lifecycle events

Existing `stream HOST NAME` commands keep their behavior. `--app-id ID`
selects the advertised GameStream application ID instead of the display name;
this handles equal and punctuation-heavy names without escaping them into a
shell. `HOST` can be the saved host UUID. Pairing stays with Pyrolight's identity
manager and certificate pins.

`--settings-revision REV` refuses to launch if saved preferences changed since
`prepare-launch`. `--integration-events` prints lines prefixed
`PYROLIGHT_EVENT ` followed by schema-1 JSON. States come from launcher/Session
signals: connecting, launching, streaming, disconnected and failed. The streaming
event includes negotiated codec, dimensions, FPS, requested transport bitrate,
chroma flags and Qt/SDL display backends. These are a connection snapshot, not
rendered-FPS, packet-loss or latency measurements. Auxiliary `quality` events report Pyrolight’s native good/poor connection signal without changing lifecycle state. Detailed performance remains
in the decoder overlay.

Integrations preserve games by launching with `--no-quit-after`. Explicit
`quit HOST --app-id ID --integration-events` checks the current application and
sends `expectedAppId` to Pyroshine's authenticated `/cancel`. A different current
application returns a conflict; standard quit without an ID keeps its behavior.
Conditional cancellation identifies applications, not successive instances of
the same application. Use a matching Pyroshine build for the host-side check.

## Catalog, artwork and network tools

`list --json HOST` retains schema 1 and launcher identities. The paired-only
catalog includes every app advertised by the host. Original Steam app IDs are
in `sources` as `steam:ID`, separate from GameStream `id`.

`list --json --artwork-id ID HOST` verifies the app is advertised and returns
`schema`, `hostUuid`, `appId` and `image` (PNG data URI, at most 300×450).
The request uses Pyrolight's authenticated host transport. Missing artwork
returns an empty image; integrations provide their own visual fallback.

`list --json --network-action check|calibrate|fix HOST` reuses NetworkBuffers
and PyroWaveCalibrator. Check is read-only. Calibration only reports a ceiling;
it never applies bitrate. Fix invokes the existing platform authorization flow
for receive buffers. Integrations must run mutation/calibration only after an
explicit user action and without their own active stream. Host-side probe
admission still rejects conflicting sessions.

## Validation

```sh
qmake6 app/tests/streamingpreferences_test.pro -o build/prefs-test/Makefile
make -C build/prefs-test
./build/prefs-test/streamingpreferences-test
qmake6 app/tests/settingsapi_test.pro -o build/settingsapi-test/Makefile
make -C build/settingsapi-test
./build/settingsapi-test/settingsapi-test
```

The tests cover registry parity, profile/global behavior, validation, stale
revisions, two concurrent CLI writers through a live owner and headless
cross-process persistence. CI runs both. Run standard and PyroWave client
builds, and the controller/input suites described in CONTRIBUTING.md.
Real SteamOS overlays/focus, suspend, codecs, displays and retained-game
reconnects require a real client and host; these tests do not establish them.
