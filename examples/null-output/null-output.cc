/* Copyright (c) 2022-2026 Stinky Computing AB. SPDX-License-Identifier: MIT (see LICENSE in the SDK folder) */

// Null outputs: outputs that count what they receive and discard it. They are
// templates for device outputs, and one library provides three types:
//
//  - com.example.null-output takes video as UYVY in host memory and audio as
//    32-bit float planar, one stereo pair per bound mix, at 48 kHz. It has a
//    JSON parameter and reports JSON status.
//  - com.example.audio-sink takes no video, and all bound mixes as one
//    24-bit interleaved multichannel stream at 96 kHz, as an SDI embedder or
//    an AES67 sender would.
//  - com.example.interlaced-sink takes interlaced UYVY, top field first: one
//    frame per two engine frames, as a 1080i SDI output would. It also
//    provides a clock, as an output card locked to a house reference does, so
//    the engine can run from it.
//
// The three types differ only in their descriptors; they share the callbacks.

#define SESAME_PLUGIN_IMPLEMENTATION
#include <sesame-plugin.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace {

constexpr uint64_t STATUS_EVERY_FRAMES = 25;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

// key, label, type, default, min, max, enum values, enum count
const sesame_param_def PARAMS[] = {
    {"logEvery", "Log every N frames (0 = off)", SESAME_PARAM_INT, "0", 0, 100000, nullptr, 0},
    // A JSON parameter arrives as compact JSON text, for settings with structure.
    {"labels", "Labels (JSON)", SESAME_PARAM_JSON, "{}", 0, 0, nullptr, 0},
};

const char* param(const sesame_param* p, uint32_t n, const char* key, const char* fallback) {
  for (uint32_t i = 0; i < n; i++) {
    if (std::strcmp(p[i].key, key) == 0) return p[i].value;
  }
  return fallback;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

struct NullOut {
  const sesame_host* host = nullptr;
  void* ctx = nullptr;
  std::atomic<uint32_t> log_every{0};  // written by update(), read on the output thread
  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> samples{0};
  std::atomic<uint64_t> data_frames{0};
};

void report_status(NullOut* o) {
  char json[160];
  std::snprintf(json, sizeof json, "{\"frames\":%llu,\"samples\":%llu,\"dataFrames\":%llu}",
                static_cast<unsigned long long>(o->frames.load()), static_cast<unsigned long long>(o->samples.load()),
                static_cast<unsigned long long>(o->data_frames.load()));
  o->host->set_status(o->ctx, SESAME_STATE_ONLINE, nullptr, json);
}

// ---------------------------------------------------------------------------
// Receiving video, audio and data
// ---------------------------------------------------------------------------

// All three callbacks run on the output's own thread, one at a time.

// `f->data` holds the frame in the descriptor's output_format, `f->height` rows `f->pitch_bytes` apart, until this
// returns. A device output copies or DMAs it to the card here.
void on_video(sesame_instance self, const sesame_output_frame* f) {
  auto* o = static_cast<NullOut*>(self);
  const auto n = ++o->frames;
  if (n % STATUS_EVERY_FRAMES == 0) report_status(o);
  const auto every = o->log_every.load();
  if (every != 0 && n % every == 0) {
    char msg[96];
    std::snprintf(msg, sizeof msg, "null-output: %llu frames, last %u (%ux%u)", static_cast<unsigned long long>(n),
                  f->frame, f->width, f->height);
    o->host->log(o->ctx, SESAME_LOG_INFO, msg);
  }
}

// Once per engine frame: per bound mix (mix_id set), or once for all mixes in the multichannel layout.
void on_audio(sesame_instance self, const sesame_output_audio* a) {
  auto* o = static_cast<NullOut*>(self);
  o->samples += a->samples;
}

void on_data(sesame_instance self, const sesame_data*) { static_cast<NullOut*>(self)->data_frames++; }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

sesame_instance create(const sesame_host* host, void* ctx, const char* /*id*/, const sesame_engine_info* /*info*/,
                       const sesame_param* p, uint32_t n) {
  auto* o = new NullOut{};
  o->host = host;
  o->ctx = ctx;
  o->log_every = static_cast<uint32_t>(std::strtoul(param(p, n, "logEvery", "0"), nullptr, 10));
  return o;
}

void destroy(sesame_instance self) { delete static_cast<NullOut*>(self); }

// Video and audio arrive on the host's output thread, so there are no threads to start or stop.
sesame_bool start(sesame_instance) { return 1; }
void stop(sesame_instance) {}

sesame_bool can_update(sesame_instance, const sesame_param*, uint32_t) { return 1; }

sesame_bool update(sesame_instance self, const sesame_param* p, uint32_t n) {
  auto* o = static_cast<NullOut*>(self);
  o->log_every = static_cast<uint32_t>(std::strtoul(param(p, n, "logEvery", "0"), nullptr, 10));
  return 1;
}

// The engine calls this on its timer thread when the instance is used as clock; it must not block. A card returns
// its reference clock; this example returns a monotonic host clock.
sesame_bool get_clock(sesame_instance, sesame_clock_time* time) {
  using namespace std::chrono;
  time->now_us = static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
  time->start_us = 0;
  return 1;
}

// ---------------------------------------------------------------------------
// Descriptors, vtables and entry
// ---------------------------------------------------------------------------

const sesame_plugin_descriptor NULL_OUTPUT = {
    .struct_size = sizeof(sesame_plugin_descriptor),
    .id = "com.example.null-output",
    .name = "Null Output",
    .version = "1.0.0",
    .kind = SESAME_PLUGIN_OUTPUT,
    .has_video = 1,
    .max_audio_channels = 16,
    .video_delivery = SESAME_VIDEO_HOST,
    .params = PARAMS,
    .param_count = std::size(PARAMS),
    .audio_delivery = SESAME_AUDIO_PCM,
    .output_format = SESAME_PIXEL_UYVY,
    .audio_layout = SESAME_AUDIO_LAYOUT_PER_MIX,
    .output_sample_format = SESAME_SAMPLE_F32,
    .output_planar = 1,
    .output_sample_rate = 0,  // the engine rate, 48 kHz
};

const sesame_plugin_descriptor AUDIO_SINK = {
    .struct_size = sizeof(sesame_plugin_descriptor),
    .id = "com.example.audio-sink",
    .name = "Audio Sink",
    .version = "1.0.0",
    .kind = SESAME_PLUGIN_OUTPUT,
    .has_video = 0,
    .max_audio_channels = 16,
    .video_delivery = SESAME_VIDEO_NONE,
    .params = PARAMS,
    .param_count = 1,  // logEvery only
    .audio_delivery = SESAME_AUDIO_PCM,
    .audio_layout = SESAME_AUDIO_LAYOUT_MULTICHANNEL,
    .output_sample_format = SESAME_SAMPLE_S24,
    .output_planar = 0,
    .output_sample_rate = 96000,
};

const sesame_plugin_descriptor INTERLACED_SINK = {
    .struct_size = sizeof(sesame_plugin_descriptor),
    .id = "com.example.interlaced-sink",
    .name = "Interlaced Sink",
    .version = "1.0.0",
    .kind = SESAME_PLUGIN_OUTPUT,
    .has_video = 1,
    .max_audio_channels = 16,
    .video_delivery = SESAME_VIDEO_HOST,
    .params = PARAMS,
    .param_count = 1,  // logEvery only
    .audio_delivery = SESAME_AUDIO_PCM,
    .output_format = SESAME_PIXEL_UYVY,
    .audio_layout = SESAME_AUDIO_LAYOUT_PER_MIX,
    .output_sample_format = SESAME_SAMPLE_F32,
    .output_planar = 1,
    .output_field_order = SESAME_FIELD_TOP_FIRST,
    .output_field_sequential = 0,
};

const sesame_output_vtable VIDEO_AND_AUDIO = {
    .struct_size = sizeof(sesame_output_vtable),
    .create = create,
    .destroy = destroy,
    .start = start,
    .stop = stop,
    .can_update = can_update,
    .update = update,
    .on_data = on_data,
    .on_video = on_video,
    .on_audio = on_audio,
};

// An output that provides the engine clock adds get_clock.
const sesame_output_vtable VIDEO_AUDIO_AND_CLOCK = {
    .struct_size = sizeof(sesame_output_vtable),
    .create = create,
    .destroy = destroy,
    .start = start,
    .stop = stop,
    .can_update = can_update,
    .update = update,
    .on_data = on_data,
    .on_video = on_video,
    .on_audio = on_audio,
    .get_clock = get_clock,
};

// An output without video leaves on_video empty.
const sesame_output_vtable AUDIO_ONLY = {
    .struct_size = sizeof(sesame_output_vtable),
    .create = create,
    .destroy = destroy,
    .start = start,
    .stop = stop,
    .can_update = can_update,
    .update = update,
    .on_data = on_data,
    .on_audio = on_audio,
};

struct Type {
  const sesame_plugin_descriptor* descriptor;
  const sesame_output_vtable* vtable;
};

const Type TYPES[] = {
    {&NULL_OUTPUT, &VIDEO_AND_AUDIO},
    {&AUDIO_SINK, &AUDIO_ONLY},
    {&INTERLACED_SINK, &VIDEO_AUDIO_AND_CLOCK},
};

sesame_bool plugin_init(const char* /*library_path*/) { return 1; }
void plugin_deinit() {}
uint32_t descriptor_count() { return std::size(TYPES); }

const sesame_plugin_descriptor* descriptor(uint32_t index) {
  return index < std::size(TYPES) ? TYPES[index].descriptor : nullptr;
}

const void* vtable(const char* id) {
  for (const auto& type : TYPES) {
    if (std::strcmp(id, type.descriptor->id) == 0) return type.vtable;
  }
  return nullptr;
}

}  // namespace

extern "C" SESAME_EXPORT const sesame_plugin_entry sesame_plugin_entry_v1 = {
    SESAME_PLUGIN_ABI_MAJOR, SESAME_PLUGIN_ABI_MINOR, plugin_init, plugin_deinit, descriptor_count, descriptor, vtable,
};
