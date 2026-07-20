# QtMeshEditor Sentry Telemetry

QtMeshEditor uses the existing `SentryReporter` wrapper for both crash reporting and anonymous product telemetry. Do not add another analytics SDK or emit telemetry directly from feature code.

## Privacy Rules

- Default PII collection is not enabled. The bundled sentry-native SDK has no `sendDefaultPii` option, and QtMeshEditor only sets an opaque `user.id`.
- The installation identifier is a random UUID created only after telemetry is enabled and stored at `telemetry/anonymousInstallationId` in `QSettings`.
- Sentry `user.id` is `install:<uuid>`. The UUID is never derived from MAC address, disk serial, hostname, username, account data, IP address, hardware fingerprint, file content, prompts or model data.
- If telemetry is disabled, QtMeshEditor does not generate or submit the anonymous installation ID.
- Reset mechanism: call `SentryReporter::resetAnonymousInstallationId()` or remove the `telemetry/anonymousInstallationId` QSettings key. Disabling telemetry through `SentryReporter::setEnabled(false)` also removes it.
- Breadcrumbs and event contexts are sanitized by `SentryReporter`; paths, filenames, prompts, emails, GitHub identifiers and tokens are redacted.
- `QTMESH_TELEMETRY_ROLE` may be `developer`, `ci` or `tester`. Any other value is submitted as `user`. This appears as `telemetry.role` so internal activity can be filtered without location data.

## Session Context

Every process creates a fresh random `session.id`; multiple GUI, CLI and MCP sessions on the same machine reuse the same anonymous installation ID when telemetry remains enabled. Common tags on telemetry events include:

- `app.version` and `release`
- `launch_mode`: `gui`, `gui+mcp`, `cli` or `mcp`
- `source_surface`: `gui`, `cli` or `mcp` when applicable
- `telemetry.role`
- `session.id`
- `os`, `arch`, `qt_version`
- `capability` for feature-specific events

Numeric values such as durations and counts live in event context rather than tags.

## Events

Application lifecycle:

- `app.startup`: emitted once after Sentry session configuration.
- `app.shutdown`: emitted on clean shutdown with `duration_ms`.

File workflow:

- `file.import.started`, `file.import.completed`, `file.import.failed`
- `file.export.started`, `file.export.completed`, `file.export.failed`

Properties: `source_surface`, `input_format`, `output_format`, `asset_kind`, `duration_ms`, `success`, `failure_category`, `model_count`, `animation_count`, `size_bucket`. Formats are extensions only; paths and filenames are never sent.

CLI and MCP:

- `cli.command.started`, `cli.command.completed`, `cli.command.failed`
- `mcp.tool.started`, `mcp.tool.completed`, `mcp.tool.failed`

Properties: `command` or `tool`, `phase`, `invocation.id`, `duration_ms`, `changed_scene`, `success`, `failure_category`, `source_surface`. MCP arguments are not submitted.

AI model management:

- `ai.model_catalog.opened`
- `ai.model_download.started`, `ai.model_download.completed`, `ai.model_download.failed`, `ai.model_download.canceled`
- `ai.model_delete.started`, `ai.model_delete.completed`, `ai.model_delete.failed`
- `ai.model_download_all.started`, `ai.model_download_all.completed`
- `ai.feature_model_missing`

Properties: `model_id`, `capability`, `source`, `duration_ms`, `byte_size_bucket`, `failure_category`, `build_available`. Model IDs are stable catalog IDs, not paths.

Editing adoption:

- `edit.mode.entered`
- `selection.mesh`
- `selection.bone`
- `transform.completed`
- `segmentation.started`, `segmentation.completed`, `segmentation.failed`
- `animation.played`
- `animation.exported`

`transform.completed` records `transform_type` and `target_kind` only after a committed edit. It does not emit continuous mouse-move telemetry.

Segmentation properties: `requested_category`, `resolved_category`, `automatic`, `manual`, `ai`, `geometric_fallback`, `result_part_count`, `duration_ms`, `success`, `failure_category`.

## Crash Diagnostics

Release workflow symbol upload currently exists for all release platforms:

- Windows: copies `QtMeshEditor.exe`, strips the shipped binary, uploads the unstripped debug executable with `sentry-cli debug-files upload --include-sources`.
- Linux: creates `QtMeshEditor.debug` with `objcopy --only-keep-debug`, strips the binary, adds a GNU debug link, uploads the debug file.
- macOS: runs `dsymutil` for `QtMeshEditor.app/Contents/MacOS/QtMeshEditor`, strips the binary, uploads the `.dSYM` bundle.

Crashes are associated with release by `sentry_options_set_release`, anonymous installation by Sentry `user.id`, and session by `session.id`. Breadcrumbs are capped by Sentry SDK defaults and sanitized before submission. Handled operational failures use telemetry events or `captureMessage(..., "error")`; fatal crashes remain native Sentry crash events.

Historical issues like `QTMESHEDITOR-D` showing only `BaseThreadInitThunk` and `QTMESHEDITOR-2` showing mostly unknown frames are most likely from one of these conditions: the crash came from a build predating debug-file upload, debug files were not uploaded because the event was not from a GitHub `release.published` workflow, the uploaded debug file did not match the binary build ID, or the Sentry event release did not match the finalized release name. Verify the event's debug image IDs against uploaded debug files in Sentry, and compare the event release to `qtmesheditor@<applicationVersion>`.

## Example Sentry Queries

Exclude internal activity in all product dashboards:

```text
!telemetry.role:developer !telemetry.role:ci
```

Weekly active anonymous installations:

```text
event.type:default message:"app.startup" !telemetry.role:developer !telemetry.role:ci
```

Group by `user.id` and use a 7-day interval.

New versus returning installations:

```text
message:"app.startup" !telemetry.role:developer !telemetry.role:ci
```

Group by `user.id`; classify first-seen users as new and users with previous `app.startup` events as returning.

Sessions per installation:

```text
message:"app.startup" !telemetry.role:developer !telemetry.role:ci
```

Group by `user.id` and count unique `session.id`.

Import to edit to export funnel:

```text
message:["file.import.completed", "edit.mode.entered", "file.export.completed"] !telemetry.role:developer !telemetry.role:ci
```

Group by `user.id` or `session.id` and use Sentry Discover or Funnels if available.

GUI versus CLI versus MCP adoption:

```text
message:["app.startup", "cli.command.completed", "mcp.tool.completed"] !telemetry.role:developer !telemetry.role:ci
```

Group by `launch_mode` and `source_surface`.

Model-download success rate:

```text
message:["ai.model_download.completed", "ai.model_download.failed", "ai.model_download.canceled"] !telemetry.role:developer !telemetry.role:ci
```

Group by `model_id` and `capability`; compute completed / (completed + failed + canceled).

Crashes per release and affected installations:

```text
event.type:error level:fatal !telemetry.role:developer !telemetry.role:ci
```

Group by `release`; count unique `user.id` for affected installations.

Exclude developer and CI traffic explicitly:

```text
!telemetry.role:developer !telemetry.role:ci
```
