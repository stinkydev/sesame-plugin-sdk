# Packaging, Installing and Testing

## The plugin folder

A plugin ships as one folder:

```
com.vendor.capture/
  capture.dll      the library (capture.so on Linux); exactly one per folder
  plugin.json      optional: how the config editor presents the plugin
  icon.svg         optional, referenced from plugin.json
  editor.js        optional custom editor panel, referenced from plugin.json
```

Types, parameters, metadata IDs and deliveries are defined by the library's
descriptors, and Sesame validates configs against them. `plugin.json` adds
presentation for the config editor. A plugin without `plugin.json` loads
normally; the editor then shows each type as a generic node with its parameters
as fields and ports for its video and audio.

## plugin.json

```json
{
  "id": "com.vendor.capture",
  "types": {
    "com.vendor.capture.input": {
      "displayName": "Vendor Capture",
      "category": "Capture",
      "description": "One input of a Vendor capture card.",
      "icon": "icon.svg",
      "params": {
        "device": { "group": "Device", "description": "Card and connector" },
        "format": { "group": "Signal" },
        "delay": { "group": "Signal", "unit": "ms", "step": 1 },
        "routing": { "multiline": true },
        "serial": { "hidden": true }
      },
      "metadata": {
        "com.vendor.capture.signal.v1": { "description": "Detected signal format and lock state" }
      }
    }
  },
  "editor": { "module": "editor.js", "editorApi": "^1.5.0", "extensionKeys": ["vendorCapture"] }
}
```

- `id` names the package. It is required when the manifest has an editor module.
- `types` is keyed by the type IDs in your descriptors. Every field is optional:
  - `displayName`: the node's title. The default is the descriptor's `name`.
  - `category`: a submenu for the type in the editor's add menu, within
    **Sources** or **Outputs**.
  - `description`: passed to the editor; not shown yet.
  - `icon`: an `.svg` or `.png` file in the folder, at most 256 KiB.
  - `params`, per parameter key: `group` (fields with the same group are shown
    together), `description`, `unit`, `step`, `hidden` (kept in the config but
    not shown), and `multiline` (a multi-line text field, for JSON parameters).
  - `metadata`, per metadata ID: `description`.
  Unknown fields are passed through to the editor untouched.
- `editor` ships a custom editor panel: an ES module (`.js` or `.mjs`, at most
  4 MiB) written against the `@stinkycomputing/sesame-editor-api` package, the
  editor API version range it needs, and the config `extensions` keys it owns.
  The module provides the editor nodes for all types in the library, in place
  of the generic nodes.

The manifest may only name what the library declares. A manifest that names a
type, parameter or metadata ID the library does not have, refers to a file
outside the folder, or is not valid JSON makes Sesame skip the whole plugin at
startup, with the reason in the log. The manifest is at most 1 MiB.

## Installing

Copy the plugin folder into a plugin directory:

- Windows: `plugins` next to `sesame.exe`.
- Linux: `/usr/lib/sesame/plugins`.
- Or any directories listed in the `SESAME_PLUGIN_DIRS` environment variable
  (separated by `;` on Windows and `:` on Linux), which replaces the default.

Sesame loads plugins at startup and keeps them loaded until it exits.
Installing or updating a plugin requires a restart. A library placed directly
in a plugin directory, outside a plugin folder, is skipped with a warning.

No further registration is needed. When a Sesame client connects to the server,
it retrieves the loaded plugin types and publishes them to the config editor,
which adds them to its add menu.

## Testing a plugin

1. Build the plugin and put it in a folder of its own with its `plugin.json`.
   The examples' CMake build does this; see the [README](../README.md).
2. Copy the folder into the plugin directory and start Sesame.
3. Check the Sesame log. Each type the library provides is listed:

   ```
   Registered source plugin 'com.vendor.capture.input' 1.0.0 from .../plugins/com.vendor.capture/capture.dll
   Native plugin types loaded: 1
   ```

   If the plugin is missing, look for a warning naming its path (see
   [Troubleshooting](#troubleshooting)).
4. Open the config editor. Sources appear under **Sources** in the add menu and
   outputs under **Outputs**, next to the built-in ones, in the manifest's
   `category` submenu if it has one. Add a node, set its parameters, connect it
   and save.
5. Watch the instance's status in the Sesame monitor: the state and text you set
   with `set_status`, your JSON detail, and the host's counters of missed, late
   and dropped frames.

To check an installation, build the examples, install the three folders, and
add a Capture Simulator to a composition.

## Configs

The config editor writes these entries for you. For reference, plugin instances
look like this in a Sesame config:

```json
{ "id": "cap", "type": "plugin", "pluginType": "com.example.capture-sim", "audioChannels": 2,
  "params": { "format": "v210", "width": 1920, "height": 1080 } }

{ "id": "sink", "type": "plugin", "pluginType": "com.example.null-output", "compositionId": "pgm",
  "audioMixIds": ["mix1"], "params": { "logEvery": 250, "labels": { "studio": "B" } } }

{ "id": "stream", "type": "plugin", "pluginType": "com.vendor.packet-sink", "encoderId": "enc1",
  "audioMixIds": ["mix1"], "params": {} }
```

- Sources set `audioChannels` (up to the descriptor's `max_audio_channels`).
  A PUSH source can set `bufferFrames`, the frames Sesame buffers to absorb
  arrival jitter, overriding the plugin's own default.
- Outputs name the composition they take (`compositionId`), or the shared
  encoder for encoded video (`encoderId`), and the audio mixes they take
  (`audioMixIds`), a stereo pair each.
- A source or output whose type provides a clock can set `useAsClock`, and the
  engine then runs from it.
- `params` holds the parameters by key, typed as JSON values.

## Troubleshooting

Sesame skips a plugin it cannot use and logs why, with the plugin's path:

| Log message | Cause |
|---|---|
| `is not in a folder of its own, skipping` | the library is directly in the plugin directory |
| `expected one shared library, found N` | the folder has no library, or more than one |
| `missing symbol sesame_plugin_entry_v1` | the entry is not exported: define `SESAME_PLUGIN_IMPLEMENTATION` before including the header, and check symbol visibility |
| `ABI major N does not match host M` | built against an incompatible version of the header |
| `init returned false` | your `init` failed |
| `descriptor struct_size too small` | `struct_size` not set to `sizeof(sesame_plugin_descriptor)` |
| `descriptor '...' ...` | a descriptor or vtable check failed; the message names the problem, such as a parameter default out of range or a missing function |
| `plugin.json: ...` | the manifest is invalid, or names something the library does not declare |
| `id '...' already registered from ...` | two plugins declare the same type ID; the first one loaded wins |
| `Plugin directory not present: ...` | the plugin directory does not exist |

Problems at run time show in the instance's status and in the log: a rejected
`set_video_format` or `set_audio_format` is logged with the reason, and frame
misses, drops and audio resyncs are counted in the status.
