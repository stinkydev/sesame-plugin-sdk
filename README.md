# Sesame Native Plugin SDK

This SDK is for building native plugins for the Sesame server. A native plugin
is a shared library that adds source or output types to Sesame, for example an
input of a capture card or an output to a playout device. Sesame loads plugins
at startup, and their types are configured in the Sesame config editor
alongside the built-in sources and outputs.

A plugin communicates with Sesame only through the C interface in
[`include/sesame-plugin.h`](include/sesame-plugin.h) and does not link against
any Sesame library.

## Contents

| Path | Contents |
|---|---|
| [`include/sesame-plugin.h`](include/sesame-plugin.h) | The plugin interface, ABI 1.0 |
| [`docs/guide.md`](docs/guide.md) | Plugin structure, threads, sources, outputs, audio, parameters, status and metadata |
| [`docs/formats.md`](docs/formats.md) | Byte layouts of the pixel and sample formats |
| [`docs/packaging.md`](docs/packaging.md) | Plugin folder, editor manifest, installation, testing and troubleshooting |
| [`examples/`](examples) | Example plugins |
| [`LICENSE`](LICENSE) | MIT License for the contents of this folder |

## Examples

| Example | Types | Covers |
|---|---|---|
| [`color-generator`](examples/color-generator/color-generator.cc) | `com.example.color-generator` | PULL source that produces a frame only when its color changes; a color parameter; state published as metadata; a control payload received |
| [`capture-sim`](examples/capture-sim/capture-sim.cc) | `com.example.capture-sim`, `com.example.stream-sim` | PUSH source that simulates a capture card: its own capture thread stamping frames and audio with the card's clock, all pixel formats, padded rows, interlace, audio in all sample formats, JSON status, and a clock the engine can run from. The stream simulator adds arrival jitter and a default buffer, as a network receiver has. The test signal is generated in [`test-pattern.cc`](examples/capture-sim/test-pattern.cc) |
| [`null-output`](examples/null-output/null-output.cc) | `com.example.null-output`, `com.example.audio-sink`, `com.example.interlaced-sink` | Outputs: UYVY video in host memory with PCM audio per mix; audio only, multichannel at 96 kHz; interlaced UYVY with a clock the engine can run from |

## Requirements

- A C or C++ compiler for Windows x64 (MSVC) or Linux x86_64 (GCC or Clang).
  The header is C; the examples are C++20.
- CMake 3.20 or later for the examples.
- For running and testing: a Sesame server with native plugin support (ABI 1.0)
  and the Sesame config editor. Sesame requires an NVIDIA GPU.
- The CUDA toolkit, only for plugins that read or write GPU memory directly.

## Quick start

Build the examples:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Each plugin is placed in its own folder under `build/Release/plugins/`. Copy
these folders into the Sesame plugin directory (`plugins` next to `sesame.exe`
on Windows, `/usr/lib/sesame/plugins` on Linux, or the directories named in the
`SESAME_PLUGIN_DIRS` environment variable) and restart Sesame. The log lists
each registered type:

```
Registered source plugin 'com.example.capture-sim' 1.0.0 from .../plugins/sesame-capture-sim/sesame-capture-sim.dll
```

In the config editor, the Capture Simulator is under **Sources > Capture** in
the add menu. [`docs/packaging.md`](docs/packaging.md) describes the steps in
more detail.

To start a new plugin, copy the example closest to it, change its IDs to your
own reverse-domain names (`com.yourcompany.product.type`), and see
[`docs/guide.md`](docs/guide.md).

## Compatibility

The interface version is `SESAME_PLUGIN_ABI_MAJOR.SESAME_PLUGIN_ABI_MINOR`, currently
1.0. Sesame loads a plugin when its major version equals Sesame's. Minor
versions add fields at the end of structs and add optional functions, so a
plugin built against an earlier minor version works with later Sesame versions.
See [Versioning](docs/guide.md#versioning).

The SDK is published at [github.com/stinkydev/sesame-plugin-sdk](https://github.com/stinkydev/sesame-plugin-sdk),
tagged with the Sesame release it shipped with (`v1.0.6-beta2`), so a plugin can be built against the SDK of the
Sesame version it targets.

## License

The contents of this folder (header, examples and documentation) are licensed
under the [MIT License](LICENSE). Plugins built with the SDK may be distributed
under any license, and example code may be copied into them. The Sesame server
is not part of the SDK.
