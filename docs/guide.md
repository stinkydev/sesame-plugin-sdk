# Writing a Sesame Plugin

This guide describes the structure of a plugin and the behaviour of the
interface. The header, [`sesame-plugin.h`](../include/sesame-plugin.h), defines
every struct and function. Byte layouts of the pixel and sample formats are in
[formats.md](formats.md); packaging, installation and testing are in
[packaging.md](packaging.md).

## Overview

Sesame renders compositions on the GPU at a fixed frame rate, the engine frame
rate, for example 50 or 59.94 frames per second. Sources supply video and audio
to compositions and audio mixers. Outputs receive composited video and mixed
audio.

A plugin library provides one or more types. Each type is a source or an
output. Instances of a type are created from the config, each with its own
parameters; two inputs of one capture card are two instances of the same source
type.

Plugin code does not run on the render thread. Sources pass frames to the host
through a ring of frame slots, and the host converts and scales them on its own
GPU stream. Outputs receive video, audio and data on a thread per output
instance.

Sources deliver video in their native pixel format and size, and audio in their
native sample format and rate. Outputs declare the formats they need. The host
performs all conversion; video conversion runs on the GPU.

The interface is C. Each struct begins with `struct_size`, and new fields are
added only at the end.

## Plugin structure

A plugin library exports `sesame_plugin_entry_v1`, of type
`sesame_plugin_entry`. The entry provides:

- `init` and `deinit`, called once when the library is loaded and unloaded.
- `descriptor_count` and `descriptor`, which return one
  `sesame_plugin_descriptor` per type. The descriptor declares the type's ID,
  kind, parameters, metadata, and the video and audio it produces or consumes.
- `vtable`, which returns the `sesame_source_vtable` or `sesame_output_vtable`
  for a type ID.

The following output receives the composite as UYVY in host memory and counts
the frames:

```c
#define SESAME_PLUGIN_IMPLEMENTATION
#include <sesame-plugin.h>

#include <stdlib.h>
#include <string.h>

typedef struct {
  const sesame_host* host;
  void* ctx;
  uint64_t frames;
} FrameCounter;

static sesame_instance create(const sesame_host* host, void* ctx, const char* id, const sesame_engine_info* info,
                              const sesame_param* params, uint32_t count) {
  FrameCounter* c = calloc(1, sizeof *c);
  if (c == NULL) return NULL;
  c->host = host;
  c->ctx = ctx;
  return c;
}

static void destroy(sesame_instance self) { free(self); }
static sesame_bool start(sesame_instance self) { return 1; }
static void stop(sesame_instance self) {}
static sesame_bool can_update(sesame_instance self, const sesame_param* p, uint32_t n) { return 1; }
static sesame_bool update(sesame_instance self, const sesame_param* p, uint32_t n) { return 1; }

static void on_video(sesame_instance self, const sesame_output_frame* f) {
  FrameCounter* c = self;
  c->frames++; /* f->data holds f->height rows of UYVY, f->pitch_bytes apart, until this returns */
}

static const sesame_plugin_descriptor DESCRIPTOR = {
    .struct_size = sizeof(sesame_plugin_descriptor),
    .id = "com.example.frame-counter",
    .name = "Frame Counter",
    .version = "1.0.0",
    .kind = SESAME_PLUGIN_OUTPUT,
    .has_video = 1,
    .video_delivery = SESAME_VIDEO_HOST,
    .output_format = SESAME_PIXEL_UYVY,
};

static const sesame_output_vtable VTABLE = {
    .struct_size = sizeof(sesame_output_vtable),
    .create = create,
    .destroy = destroy,
    .start = start,
    .stop = stop,
    .can_update = can_update,
    .update = update,
    .on_video = on_video,
};

static sesame_bool init(const char* library_path) { return 1; }
static void deinit(void) {}
static uint32_t descriptor_count(void) { return 1; }
static const sesame_plugin_descriptor* descriptor(uint32_t index) { return index == 0 ? &DESCRIPTOR : NULL; }
static const void* vtable(const char* id) { return strcmp(id, DESCRIPTOR.id) == 0 ? &VTABLE : NULL; }

SESAME_EXPORT const sesame_plugin_entry sesame_plugin_entry_v1 = {
    SESAME_PLUGIN_ABI_MAJOR, SESAME_PLUGIN_ABI_MINOR, init, deinit, descriptor_count, descriptor, vtable,
};
```

Define `SESAME_PLUGIN_IMPLEMENTATION` before including the header in the file
that defines the entry, so that the entry is declared for export. Build with
symbols hidden by default, as the example CMake files do, so that the entry is
the only exported symbol.

**IDs.** Type IDs are reverse-domain names and must be globally unique, for
example `com.yourcompany.capture.input`. Configs store them, so a released ID
must stay the same. Metadata IDs use the same form with a version suffix, for
example `com.yourcompany.capture.signal.v1`.

**Validation.** When Sesame loads a library it validates every descriptor and
vtable before registering any type: IDs and names, parameter definitions and
defaults, and the functions required by each declared capability. If any check
fails, Sesame skips the whole library and logs the reason (see
[Troubleshooting](packaging.md#troubleshooting)).

## Lifecycle and threads

Each instance goes through these calls:

1. `create`, on the API thread, when the config adds the instance. It receives
   the `sesame_host` function table, an opaque `ctx` to pass to every host
   function, the instance's node ID, a `sesame_engine_info` (frame rate, render
   size, audio sample rate and the instance's channel count) and the validated
   parameters. Allocate the instance's resources here. Returning NULL fails the
   config command.
2. `start`, on the instance's worker thread. It must return without blocking;
   plugins that need their own threads start them here.
3. Callbacks while the instance runs, as described below, and `update` when the
   config changes.
4. `stop`, which stops and joins the plugin's threads. It can be called more
   than once.
5. `destroy`, which frees the instance. It is called only after `stop`.

The host guarantees:

- No callback runs after `stop` returns, and `destroy` does not overlap a
  callback.
- The `sesame_host` pointer and `ctx` remain valid until `destroy` returns.
  Other pointers passed to a callback are valid only for the duration of that
  call.
- Host functions documented as callable from any thread can be called from any
  plugin thread between `create` and the return of `stop`.

The plugin must keep everything it returns to the host (descriptors, vtables,
parameter definitions and strings) valid from `init` until `deinit`. Static
storage meets this requirement.

Allocate buffers in `create` or when the signal format changes, and reuse them
for every frame.

## Parameters

Parameters are declared in the descriptor as `sesame_param_def` entries. The
config editor shows them as fields. The host validates them and passes them to
`create` and `update` as strings:

| Type | Value | Validation |
|---|---|---|
| `STRING` | text | none |
| `INT`, `FLOAT` | decimal text | between `min` and `max` |
| `BOOL` | `true` or `false` | |
| `COLOR` | `#rrggbb` or `#rrggbbaa`, lower case | |
| `ENUM` | one of the declared `value`s | the editor displays the `label`s |
| `JSON` | compact JSON text | must parse; for nested or list-valued settings such as routing tables |

Unknown keys are rejected and missing keys are set to their defaults, so the
plugin always receives the complete set.

`update` receives the complete set on every config change. It can run
concurrently with the plugin's threads; store new values atomically, for
example with atomics or a double buffer swapped under a lock. If `update`
returns 0, the host reports the failure and the instance keeps its previous
values. If a change requires a new instance, for example a different device,
return 0 from `can_update`; the host then stops, destroys and recreates the
instance with the new parameters.

## Status and logging

`set_status` can be called from any thread. It sets:

- the state: `OFFLINE`, `INITIALIZING`, `ONLINE` or `ERROR`;
- a line of text of at most 256 bytes, for example `1080i50, locked`;
- optionally, a JSON object of at most 64 KiB with type-specific detail such as
  signal format, buffer levels or link statistics, shown by monitoring clients.

A started instance reports `ONLINE` until the first call. The host appends its
counters of missed, late and dropped frames to the text.

`log` writes a message to the Sesame log at the given level.

## Sources

### Frame layout

A source delivers frames in its own size and pixel format. Call
`set_video_format` from `create`, and from any thread when the input format
changes. Slots acquired after the call use the new layout; slots already
acquired keep the layout they were acquired with. The default is RGBA8 at the
render size.

`sesame_video_format` contains:

- `width` and `height`, each at most 8192. Formats with chroma subsampling
  require an even width; 4:2:0 formats also require an even height.
- `pixel_format`, one of the layouts in [formats.md](formats.md).
- `matrix` (BT.709, BT.601 or BT.2020) and `range` (limited or full), for YUV
  formats and R210.
- `pitch_bytes`, the row pitch of the first plane. 0 selects the minimum pitch
  for the format. A non-zero value must be at least the minimum and a multiple
  of 4. The other planes follow from it.
- `field_order` and `field_sequential`, for interlaced frames.
- `transfer` and `primaries`. This version of Sesame composites in SDR BT.709
  and accepts only `SESAME_TRANSFER_SDR` with `SESAME_PRIMARIES_BT709`.

`set_video_format` returns 0 if the format is not accepted. The host scales
frames to the source's texture size in the composition.

Every slot carries its own `width`, `height`, `pixel_format`, `pitch_bytes` and
`size_bytes` (all planes). Write each frame according to these fields.

A slot has two buffers: `host_data` in pinned host memory and `device_data` in
GPU memory. Fill one and pass the matching flag when returning the slot:
`SESAME_FRAME_FROM_HOST`, after which the host uploads the frame, or
`SESAME_FRAME_FROM_DEVICE`. GPU writes to `device_data` must be enqueued on
`slot->stream` or be complete when the slot is returned.

**Alpha.** RGBA8 and BGRA8 carry alpha; UYVA and PA16 carry a key plane. Set
`slot->alpha_mode` to `SESAME_ALPHA_PREMULTIPLIED` if the colour is
premultiplied by alpha. The default is straight alpha. The other YUV formats are
opaque.

**Timecode.** `timecode_frames` and `timecode_valid` on a slot attach a
timecode to the frame.

### PULL and PUSH

The descriptor's `source_mode` selects one of two modes.

In **PULL** mode (`SESAME_SOURCE_PULL`) the host runs a worker thread and calls
`produce(self, slot)` once per engine frame, two frames ahead of rendering. This
mode suits generators and other sources driven by the engine.

- `slot->frame` is the engine frame to produce, and `slot->budget_us` the time
  remaining until it is needed.
- Fill a buffer and return `SESAME_FRAME_FROM_HOST` or
  `SESAME_FRAME_FROM_DEVICE`. Returning `SESAME_FRAME_NONE` keeps the previous
  frame; the colour generator example does this while its colour is unchanged.
- A source with audio calls `write_audio` once inside `produce`, with exactly
  `slot->audio_samples_needed` samples per channel at the source's audio rate.
  The per-frame counts sum exactly to the sample rate. If `write_audio` is not
  called, the host writes silence and counts an underrun.
- If `produce` returns after the deadline, the previous frame is shown again and
  a miss is counted in the status.

A PULL source without video receives `produce` calls with slots that have no
buffers, for its audio.

In **PUSH** mode (`SESAME_SOURCE_PUSH`) the plugin runs its own threads, started
in `start` and joined in `stop`. This mode suits devices and streams. For each
frame:

1. Call `acquire_frame`. It returns 0 when all slots are in use; retry after a
   short wait, or skip the frame.
2. Fill the slot. Set `slot->frame` to the engine frame the frame belongs to, or
   for a DEVICE-timed source `slot->device_time_us` to its device time (see
   [Placing source frames](#placing-source-frames)).
3. Call `submit_frame` with the `SESAME_FRAME_FROM_*` flag, or `release_frame` to
   return the slot unused. Submit slots in the order they were acquired.

Audio is passed with `push_audio`, together with the time of its first sample
(see [Source audio timing](#source-audio-timing)).

The ring has `ring_depth` slots: 4 by default, at most 16. In both modes a
frame reaches the screen one frame plus the ring's buffering after it is
produced.

### Interlaced sources

For an interlaced signal, set `field_order` to `SESAME_FIELD_TOP_FIRST` or
`SESAME_FIELD_BOTTOM_FIRST` and submit one slot per interlaced frame, on even
engine frames, for example 1080i50 into a 50 fps engine. The host shows the
first field on the slot's engine frame and the second field on the following
engine frame, each line-doubled.

Fields are interleaved row by row by default. With `field_sequential` set, the
rows of the first field are stored first, followed by the rows of the second
field, as field-based transports such as SMPTE ST 2110 deliver them.
Field-sequential storage is not supported for 4:2:0 formats.

### Source audio

Audio passes the interface in the plugin's sample format, layout and rate (see
[formats.md](formats.md#audio)). The engine mixes 32-bit float at 48 kHz; the
host converts and resamples in both directions.

Call `set_audio_format` from `create`, and when the input changes, with the
rate, sample format, layout (interleaved or planar) and a channel count equal to
the instance's configured audio channels. The default is 16-bit interleaved at
48 kHz. How audio is timed is described in
[Source audio timing](#source-audio-timing).

## Outputs

### Video delivery

The descriptor's `video_delivery` selects how an output receives video. All
output callbacks run on the output's thread, one at a time.

| `video_delivery` | Delivery |
|---|---|
| `SESAME_VIDEO_NONE` | no video; audio only |
| `SESAME_VIDEO_HOST` | `on_video` with the frame in pinned host memory, in `output_format` |
| `SESAME_VIDEO_DEVICE` | `on_video` with the frame in GPU memory, in `output_format`. GPU work that reads the frame is enqueued on `frame->stream`; the host waits for the stream before reusing the buffer |
| `SESAME_VIDEO_ENCODED` | `on_encoded_video` with compressed packets from the shared Sesame encoder named by `encoderId` in the instance's config |

`output_format` can be any pixel format listed in [formats.md](formats.md).
Frames are at the render size and minimum pitch. YUV and R210 output is BT.709
limited range. UYVA and PA16 carry the composite's alpha in their key plane.
Formats with chroma subsampling require an even render size. The frame data is
valid until `on_video` returns.

`sesame_output_frame` contains the engine frame number, size, format, pitch, the
composite's timecode, and `clock_time_us`, the time on the engine clock at which
the frame starts (see [Output timing](#output-timing)).

If an output does not keep up, the host drops frames as it takes them from the
renderer and counts the drops in the output's status. The host holds at most
three video frames per output.

### Interlaced outputs

With `output_field_order` set to `SESAME_FIELD_TOP_FIRST` or
`SESAME_FIELD_BOTTOM_FIRST`, the output receives interlaced frames. Each frame
contains two engine frames: the first field from an even engine frame and the
second field from the next one. A 50 fps engine produces 1080i50. `on_video` is
called once per frame, with `frame` set to the first field's engine frame
(always even) and the first field's timecode. With `output_field_sequential`
set, each field's rows are stored together, first field first.

Interlaced output requires a format without vertical chroma subsampling, which
excludes NV12, P010 and I420, and an even render height. The
[`interlaced-sink`](../examples/null-output/null-output.cc) example type has the
corresponding descriptor.

### Audio delivery

With `audio_delivery = SESAME_AUDIO_PCM`, `on_audio` delivers the bound audio
mixes once per engine frame, after the frame's video, in the format set in the
descriptor:

- `output_sample_format` and `output_planar`;
- `output_sample_rate`, or 0 for 48 kHz;
- `audio_layout`. With `SESAME_AUDIO_LAYOUT_PER_MIX`, `on_audio` is called once
  per bound mix with its stereo pair and the mix ID. With
  `SESAME_AUDIO_LAYOUT_MULTICHANNEL`, it is called once with the pairs of all
  bound mixes in order and a null mix ID, for example 16 channels for SDI
  embedding.

Each `sesame_output_audio` contains `clock_time_us`, the time on the engine clock
of its first sample. Interlaced outputs receive audio once per engine frame,
which is two `on_audio` calls per video frame.

Outputs with encoded video can set `audio_delivery = SESAME_AUDIO_OPUS` to
receive one Opus stream of all bound mixes in `on_encoded_audio`.

An output instance receives its channel count in `sesame_engine_info` at
`create`. A config change to the number of bound mixes recreates the instance.

## Timing and synchronization

This section describes how Sesame keeps time, and how sources and outputs relate
their own timing to it.

### Time bases

The interface uses four time bases.

| Time base | What it is | Where it appears |
|---|---|---|
| Engine frame number | Counts engine frames from the start of the engine. 32 bits. | `slot->frame`, `sesame_output_frame.frame`, `get_frame_info`, every `*_for_frame` function |
| Engine clock time | Microseconds on the clock the engine's frame timer runs from (see [The engine clock](#the-engine-clock)). With a PTP clock, this is PTP time. | `clock_time_for_frame`, `sesame_output_frame.clock_time_us`, `sesame_output_audio.clock_time_us` |
| Engine audio time | Microseconds of engine audio since the start of the engine, derived from the frame number at 48 kHz. | `audio_timestamp_for_frame`, `push_audio` from ENGINE-timed sources |
| Device time | Microseconds on a source's own clock, with any origin. | `slot->device_time_us`, `push_audio` from DEVICE-timed sources |

`get_frame_info` returns the next engine frame number and the time elapsed in
the current frame on the engine clock. `clock_time_for_frame` and
`audio_timestamp_for_frame` can be called by any instance, from any thread.

### The engine clock

The engine renders one frame per frame period of the engine clock. By default
the engine clock is a monotonic clock of the host. A source or an output can
provide the engine clock instead:

1. Set `provides_clock` in the descriptor and implement `get_clock` in the
   source or output vtable. It returns the current time in microseconds,
   `now_us`, and the clock value `start_us` at which frame 0 started.
   `get_clock` is called on the timer thread and must return without blocking.
2. The operator enables **Use as clock** on one instance. The engine's frame
   timer then runs from that instance's clock.

Engine frames start at whole multiples of the frame period after `start_us`.
For an absolute clock, return `start_us = 0`: frames then start at whole
multiples of the frame period since the clock's epoch.

A capture or playout card locked to a house reference provides its reference as
the clock, so that the engine runs in step with the card. The capture simulator
and the interlaced sink example types both provide a clock.

### PTP

A source or output locked to PTP (IEEE 1588, SMPTE ST 2059) can provide PTP
time as the engine clock: `now_us` is PTP time in microseconds and `start_us` is 0. Engine
frames then start on the ST 2059 alignment points, and engine clock time is PTP
time.

- **Receivers** compare the RTP timestamps of incoming media with
  `clock_time_for_frame` to assign it to engine frames, and use ENGINE timing.
- **Senders** derive RTP timestamps from `clock_time_us` of each output frame and
  audio block.

When one instance provides the clock, every other PTP-locked source and output
in the same Sesame shares it.

### Placing source frames

The descriptor's `source_timing` selects how a PUSH source's frames are assigned
to engine frames.

**ENGINE timing** (`SESAME_TIMING_ENGINE`, the default). The plugin sets
`slot->frame` itself. A capture thread uses `get_frame_info` to find the next
engine frame; the capture simulator example assigns each captured frame to the
frame after next, which leaves a full frame period for filling and uploading the
slot. This mode suits sources that run from the engine clock, or provide it, or
share it through PTP. If the source's clock and the engine clock differ, the
source must drop or repeat frames itself.

**DEVICE timing** (`SESAME_TIMING_DEVICE`, PUSH sources only). The plugin stamps
each frame with `slot->device_time_us` and passes audio to `push_audio` with
device time. The host measures the device clock against the engine clock: first
with a line fitted through about fifteen seconds of arrivals, then with a slow
tracking loop. Each frame is placed on the engine frame nearest to its mapped
time plus a buffer. A device clock that runs fast or slow costs a dropped or
repeated frame at even intervals, one every 200 seconds at 100 ppm and 50 fps.
Interlaced sources move in whole frames, so the top field stays on even engine
frames. This mode suits network receivers and devices that are not locked to the
engine clock.

The buffer of a DEVICE-timed source absorbs arrival jitter. The plugin requests
a default with `set_target_buffer` from `create`, for example more for a network
receiver than for a local card. The operator's `bufferFrames` setting in the
source's config takes precedence. The buffer is at least 2 frames, rounded up to
an even count for interlaced sources, and the host does not change it.

The source's status reports the timing in `timing`: the clock state (acquiring
or locked), the measured drift in ppm, the buffer and its target, and counts of
frames that arrived late (the previous frame was shown), were dropped, or were
repeated. Late frames indicate jitter larger than the buffer. The stream
simulator example type, `com.example.stream-sim`, demonstrates the mode with a
configurable clock offset, jitter and buffer.

### Source audio timing

- **Audio locked to video**, such as SDI embedded audio, with ENGINE timing.
  Each frame carries its exact share of samples. Push each frame's samples with
  the engine audio time of its frame, from `audio_timestamp_for_frame`, and leave
  `drift_compensation` off. When the source drops or repeats a frame, its audio
  is dropped or repeated with it.
- **Audio with an independent clock**, such as AES67 and SMPTE ST 2110-30
  streams, NDI, and USB or analog interfaces, with ENGINE timing. Set
  `drift_compensation` in the audio format (PUSH sources only) and push audio
  with engine audio times. The host writes the audio contiguously from the first
  timestamp and uses later timestamps as the target position. Small differences
  are corrected by gradual resampling. After a large jump the host restarts the
  audio at the pushed timestamp and counts a resync in the status.
- **DEVICE timing.** Push audio with device time. The host maps it with the
  measured clock and resamples it at the measured rate, contiguously.

### Output timing

An output receives each frame with its engine frame number and its engine clock
time, `clock_time_us`, and each block of audio with the engine clock time of its
first sample. An interlaced output's frame carries the time of its first field.
Video and audio are delivered once the frame is rendered, so an output that
paces its own transmission, such as an ST 2110 sender, schedules against these
times plus its own latency.

### Choosing a setup

| Source or output | Timing | Setup |
|---|---|---|
| Generator | engine | PULL |
| SDI or HDMI card locked to the house reference | engine; the card is the clock | PUSH, ENGINE timing, `provides_clock` with **Use as clock**, audio locked to video |
| SDI or HDMI card not locked to the engine clock | the card's own clock | PUSH, DEVICE timing, audio in device time |
| SMPTE ST 2110 receiver, facility on PTP | PTP | PUSH, ENGINE timing; one instance provides PTP as the clock (`start_us = 0`); frames assigned by RTP timestamp with `clock_time_for_frame` |
| SRT, NDI or other network receiver | the sender's clock | PUSH, DEVICE timing, `set_target_buffer` for the network's jitter |
| AES67 or ST 2110-30 audio, engine not on PTP | the stream's clock | PUSH, `drift_compensation`, or DEVICE timing |
| SMPTE ST 2110 sender | PTP | RTP timestamps from `clock_time_us` |
| SDI output card locked to the house reference | engine; the card is the clock | `provides_clock` on the output with **Use as clock** |
| SDI output card, unlocked | engine | Frames as delivered |

## Metadata and control

Plugins can publish data to clients and receive control messages. Each
metadata ID is declared in the descriptor as a `sesame_metadata_def`, with
`receive = 0` for published data and `receive = 1` for received data.

- **Publishing.** `publish_metadata` can be called from any thread with a
  declared ID and a JSON or binary payload of at most 1 MiB.
  `SESAME_PUBLISH_STATE` keeps the latest value per ID and sends it to clients
  that connect later. `SESAME_PUBLISH_EVENT` sends the payload once. The
  config's metadata bindings determine which streaming outputs carry the data.
- **Receiving.** A control message addressed to the instance's node ID is
  delivered to `on_data` with its `metadata_id` and `sender`, if the config's
  bindings allow that sender to control the node.
- **Engine state.** An output bound to engine state also receives the engine's
  state frames in `on_data`, with `track` set and `type` identifying the
  message.

`on_data` is called on a host thread of the instance: for a PULL source on the
worker thread between `produce` calls, for a PUSH source on a host thread
concurrent with the plugin's threads, and for an output on the output thread
between frames. The host queues at most 64 items per instance and counts drops
in the status.

The colour generator example publishes its colour as state and accepts a
set-colour payload.

## Versioning

The interface version is `SESAME_PLUGIN_ABI_MAJOR.SESAME_PLUGIN_ABI_MINOR`,
currently 1.0. The library reports the version it was built against in its
entry.

- Sesame loads a library only if its major version equals Sesame's.
- Minor versions add fields at the end of structs, enum values and optional
  functions. A plugin built against an earlier minor version works with later
  Sesame versions, which treat fields unknown to the plugin as zero.
- Set `struct_size` to the `sizeof` of each struct the plugin fills, as defined
  by the header it is built against.
- `sesame_engine_info.abi_minor` reports the minor version of the running
  Sesame.

The descriptor's `version` string is the plugin's own version. Sesame shows it
in the log and the type list.
