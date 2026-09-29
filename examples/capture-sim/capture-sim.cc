/* Copyright (c) 2022-2026 Stinky Computing AB. SPDX-License-Identifier: MIT (see LICENSE in the SDK folder) */

// Capture simulator: a PUSH source that behaves like one input of a capture
// card. It demonstrates:
//
//  - declaring the signal with set_video_format and set_audio_format, in any
//    pixel format, size, row padding and scan, and changing it at runtime;
//  - a capture thread of the plugin's own that assigns each frame to an engine
//    frame with get_frame_info, then acquires, fills and submits a slot;
//  - interlaced frames, submitted on even engine frames only;
//  - audio pushed with engine timestamps, either locked to the video like SDI
//    embedded audio or on an independent clock like an AES67 stream;
//  - status text and JSON, and a clock the engine can run from.
//
// The library's second type, com.example.stream-sim, is a DEVICE-timed source
// such as a network receiver: it stamps frames and audio with its own clock,
// which runs clockPpm fast or slow, delivers them up to jitterMs late, and
// requests a default buffer with set_target_buffer. The host measures the
// clock, buffers the frames and places them on engine frames.
//
// The pixels and samples come from test-pattern.h. A real plugin gets them
// from its hardware.

#define SESAME_PLUGIN_IMPLEMENTATION
#include <sesame-plugin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <random>
#include <thread>
#include <vector>

#include "test-pattern.h"

namespace {

using capture_sim::BarPainter;
using capture_sim::ToneGenerator;

constexpr uint32_t STATUS_EVERY_FRAMES = 25;

// ---------------------------------------------------------------------------
// Parameters
// ---------------------------------------------------------------------------

const sesame_enum_value FORMAT_VALUES[] = {
    {"rgba", "RGBA 8-bit"},
    {"bgra", "BGRA 8-bit"},
    {"uyvy", "UYVY 4:2:2 8-bit"},
    {"yuyv", "YUYV 4:2:2 8-bit"},
    {"uyva", "UYVA 4:2:2:4 8-bit"},
    {"v210", "v210 4:2:2 10-bit"},
    {"nv12", "NV12 4:2:0 8-bit"},
    {"p010", "P010 4:2:0 10-bit"},
    {"p216", "P216 4:2:2 16-bit"},
    {"pa16", "PA16 4:2:2:4 16-bit"},
    {"i420", "I420 4:2:0 8-bit"},
    {"r210", "r210 RGB 10-bit"},
    {"rfc4175", "RFC 4175 4:2:2 10-bit"},
};

// The pixel format behind each FORMAT_VALUES entry, in the same order.
const sesame_pixel_format FORMATS[] = {
    SESAME_PIXEL_RGBA8, SESAME_PIXEL_BGRA8, SESAME_PIXEL_UYVY, SESAME_PIXEL_YUYV, SESAME_PIXEL_UYVA,
    SESAME_PIXEL_V210,  SESAME_PIXEL_NV12,  SESAME_PIXEL_P010, SESAME_PIXEL_P216, SESAME_PIXEL_PA16,
    SESAME_PIXEL_I420,  SESAME_PIXEL_R210,  SESAME_PIXEL_RFC4175_422_10,
};
static_assert(std::size(FORMATS) == std::size(FORMAT_VALUES));

const sesame_enum_value SCAN_VALUES[] = {
    {"progressive", "Progressive"},
    {"tff", "Interlaced, top field first"},
    {"bff", "Interlaced, bottom field first"},
};

const sesame_enum_value SAMPLE_FORMAT_VALUES[] = {
    {"s16", "16-bit"}, {"s24", "24-bit"}, {"s32", "32-bit"}, {"f32", "32-bit float"}, {"l24", "L24 (AES67)"},
    {"l16", "L16 (AES67)"},
};

const sesame_enum_value SAMPLE_RATE_VALUES[] = {
    {"44100", "44.1 kHz"},
    {"48000", "48 kHz"},
    {"96000", "96 kHz"},
};

const sesame_enum_value AUDIO_CLOCK_VALUES[] = {
    {"locked", "Locked to video (SDI)"},
    {"independent", "Independent (AES67, NDI)"},
};

// key, label, type, default, min, max, enum values, enum count
const sesame_param_def PARAMS[] = {
    {"speed", "Bar speed (px/frame)", SESAME_PARAM_INT, "4", 0, 1024, nullptr, 0},
    {"tone", "1 kHz tone", SESAME_PARAM_BOOL, "true", 0, 0, nullptr, 0},
    {"format", "Pixel format", SESAME_PARAM_ENUM, "rgba", 0, 0, FORMAT_VALUES, std::size(FORMAT_VALUES)},
    {"width", "Width (0 = render size)", SESAME_PARAM_INT, "0", 0, 8192, nullptr, 0},
    {"height", "Height (0 = render size)", SESAME_PARAM_INT, "0", 0, 8192, nullptr, 0},
    {"pitchPad", "Row padding (bytes, multiple of 4)", SESAME_PARAM_INT, "0", 0, 4096, nullptr, 0},
    {"scan", "Scan", SESAME_PARAM_ENUM, "progressive", 0, 0, SCAN_VALUES, std::size(SCAN_VALUES)},
    {"fieldSequential", "Fields stored one after the other", SESAME_PARAM_BOOL, "false", 0, 0, nullptr, 0},
    {"alpha", "Alpha for formats with a key", SESAME_PARAM_INT, "255", 0, 255, nullptr, 0},
    {"audioFormat", "Audio sample format", SESAME_PARAM_ENUM, "s16", 0, 0, SAMPLE_FORMAT_VALUES,
     std::size(SAMPLE_FORMAT_VALUES)},
    {"audioPlanar", "Planar audio", SESAME_PARAM_BOOL, "false", 0, 0, nullptr, 0},
    {"audioRate", "Audio sample rate", SESAME_PARAM_ENUM, "48000", 0, 0, SAMPLE_RATE_VALUES,
     std::size(SAMPLE_RATE_VALUES)},
    {"audioClock", "Audio clock", SESAME_PARAM_ENUM, "locked", 0, 0, AUDIO_CLOCK_VALUES, std::size(AUDIO_CLOCK_VALUES)},
    {"audioClockPpm", "Independent clock offset (ppm)", SESAME_PARAM_FLOAT, "0", -10000, 10000, nullptr, 0},
};

const sesame_param_def STREAM_PARAMS[] = {
    {"clockPpm", "Clock offset (ppm)", SESAME_PARAM_FLOAT, "0", -10000, 10000, nullptr, 0},
    {"jitterMs", "Arrival jitter (ms)", SESAME_PARAM_FLOAT, "0", 0, 1000, nullptr, 0},
    {"bufferFrames", "Buffer it asks for (frames)", SESAME_PARAM_INT, "3", 2, 60, nullptr, 0},
    {"speed", "Bar speed (px/frame)", SESAME_PARAM_INT, "4", 0, 1024, nullptr, 0},
    {"tone", "1 kHz tone", SESAME_PARAM_BOOL, "true", 0, 0, nullptr, 0},
    {"format", "Pixel format", SESAME_PARAM_ENUM, "rgba", 0, 0, FORMAT_VALUES, std::size(FORMAT_VALUES)},
    {"width", "Width (0 = render size)", SESAME_PARAM_INT, "0", 0, 8192, nullptr, 0},
    {"height", "Height (0 = render size)", SESAME_PARAM_INT, "0", 0, 8192, nullptr, 0},
    {"scan", "Scan", SESAME_PARAM_ENUM, "progressive", 0, 0, SCAN_VALUES, std::size(SCAN_VALUES)},
    {"audioFormat", "Audio sample format", SESAME_PARAM_ENUM, "s16", 0, 0, SAMPLE_FORMAT_VALUES,
     std::size(SAMPLE_FORMAT_VALUES)},
    {"audioRate", "Audio sample rate", SESAME_PARAM_ENUM, "48000", 0, 0, SAMPLE_RATE_VALUES,
     std::size(SAMPLE_RATE_VALUES)},
};

/** A parameter's value. The host always passes every declared parameter, so the fallback is for safety only. */
const char* param(const sesame_param* p, uint32_t n, const char* key, const char* fallback) {
  for (uint32_t i = 0; i < n; i++) {
    if (std::strcmp(p[i].key, key) == 0) return p[i].value;
  }
  return fallback;
}

bool param_bool(const sesame_param* p, uint32_t n, const char* key) {
  return std::strcmp(param(p, n, key, "false"), "true") == 0;
}

sesame_pixel_format parse_format(const char* name) {
  for (size_t i = 0; i < std::size(FORMAT_VALUES); i++) {
    if (std::strcmp(name, FORMAT_VALUES[i].value) == 0) return FORMATS[i];
  }
  return SESAME_PIXEL_RGBA8;
}

sesame_field_order parse_scan(const char* name) {
  if (std::strcmp(name, "tff") == 0) return SESAME_FIELD_TOP_FIRST;
  if (std::strcmp(name, "bff") == 0) return SESAME_FIELD_BOTTOM_FIRST;
  return SESAME_FIELD_PROGRESSIVE;
}

sesame_sample_format parse_sample_format(const char* name) {
  if (std::strcmp(name, "s24") == 0) return SESAME_SAMPLE_S24;
  if (std::strcmp(name, "s32") == 0) return SESAME_SAMPLE_S32;
  if (std::strcmp(name, "f32") == 0) return SESAME_SAMPLE_F32;
  if (std::strcmp(name, "l24") == 0) return SESAME_SAMPLE_L24;
  if (std::strcmp(name, "l16") == 0) return SESAME_SAMPLE_L16;
  return SESAME_SAMPLE_S16;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

struct CaptureSim {
  const sesame_host* host = nullptr;
  void* ctx = nullptr;
  sesame_engine_info info{};

  // Settings the capture thread reads while update() may change them.
  std::atomic<int> speed{4};
  std::atomic<bool> tone{true};
  std::atomic<uint32_t> alpha{255};
  std::atomic<bool> interlaced{false};

  // Capture thread and its state.
  std::atomic<bool> running{false};
  std::thread worker;
  BarPainter bars;
  ToneGenerator tone_generator;
  std::vector<uint8_t> pcm;  // one frame of audio, reused
  sesame_audio_format audio{};
  double audio_clock = 1.0;      // device audio clock against the engine clock, from audioClockPpm
  double audio_remainder = 0.0;  // fractional samples carried to the next frame

  std::atomic<uint64_t> frames{0};
  std::atomic<uint64_t> dropped{0};

  // Stream simulator only: its own clock, arrival jitter, and random delays from a fixed seed.
  bool stream = false;
  double clock_ppm = 0.0;
  int64_t jitter_us = 0;
  std::mt19937 random{42};
};

// ---------------------------------------------------------------------------
// Signal format
// ---------------------------------------------------------------------------

/** Minimum first-plane pitch of a format, as the host computes it; row padding is added on top. */
uint32_t min_pitch(sesame_pixel_format format, uint32_t width) {
  switch (format) {
    case SESAME_PIXEL_RGBA8:
    case SESAME_PIXEL_BGRA8:
      return width * 4;
    case SESAME_PIXEL_R210:
      return ((width + 63) / 64) * 256;
    case SESAME_PIXEL_V210:
      return ((width + 47) / 48) * 128;
    case SESAME_PIXEL_NV12:
    case SESAME_PIXEL_I420:
      return width;
    case SESAME_PIXEL_RFC4175_422_10:
      return (width / 2) * 5;
    default:
      return width * 2;
  }
}

/**
 * Declares the video and audio formats the parameters select, as a capture
 * plugin does when it detects its input signal. Slots acquired afterwards have
 * the new layout. Returns 0 if the host rejects a format.
 */
sesame_bool apply_format(CaptureSim* s, const sesame_param* p, uint32_t n) {
  const auto width = static_cast<uint32_t>(std::strtoul(param(p, n, "width", "0"), nullptr, 10));
  const auto height = static_cast<uint32_t>(std::strtoul(param(p, n, "height", "0"), nullptr, 10));
  const auto pad = static_cast<uint32_t>(std::strtoul(param(p, n, "pitchPad", "0"), nullptr, 10));

  sesame_video_format video{};
  video.struct_size = sizeof(video);
  video.width = width != 0 ? width : s->info.render_width;
  video.height = height != 0 ? height : s->info.render_height;
  video.pixel_format = parse_format(param(p, n, "format", "rgba"));
  video.matrix = SESAME_MATRIX_BT709;
  video.range = SESAME_RANGE_LIMITED;
  // A padded pitch must be a multiple of 4, so the minimum is rounded up before the padding is added.
  video.pitch_bytes = pad == 0 ? 0 : ((min_pitch(video.pixel_format, video.width) + 3) & ~3u) + pad;
  video.field_order = parse_scan(param(p, n, "scan", "progressive"));
  video.field_sequential = param_bool(p, n, "fieldSequential") ? 1 : 0;
  if (!s->host->set_video_format(s->ctx, &video)) return 0;
  s->interlaced = video.field_order != SESAME_FIELD_PROGRESSIVE;
  s->alpha = static_cast<uint32_t>(std::strtoul(param(p, n, "alpha", "255"), nullptr, 10));

  if (s->info.audio_channels == 0) return 1;
  sesame_audio_format audio{};
  audio.struct_size = sizeof(audio);
  audio.sample_rate = static_cast<uint32_t>(std::strtoul(param(p, n, "audioRate", "48000"), nullptr, 10));
  audio.channels = s->info.audio_channels;  // must equal the instance's configured channel count
  audio.format = parse_sample_format(param(p, n, "audioFormat", "s16"));
  audio.planar = param_bool(p, n, "audioPlanar") ? 1 : 0;
  // Audio on its own clock asks the host to follow its timestamps by resampling.
  const bool independent = std::strcmp(param(p, n, "audioClock", "locked"), "independent") == 0;
  audio.drift_compensation = independent ? 1 : 0;
  if (!s->host->set_audio_format(s->ctx, &audio)) return 0;
  s->audio = audio;
  s->audio_clock = independent ? 1.0 + (std::strtod(param(p, n, "audioClockPpm", "0"), nullptr) / 1e6) : 1.0;
  return 1;
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------

/**
 * Pushes one engine frame's worth of tone, stamped with the engine time the
 * frame's audio starts at. With audioClockPpm the simulated device clock runs
 * fast or slow, so it delivers slightly more or fewer samples per engine frame
 * than the nominal rate.
 */
void push_frame_audio(CaptureSim* s, uint32_t frame) {
  const sesame_audio_format& f = s->audio;
  if (f.channels == 0) return;
  const int64_t start_us = s->host->audio_timestamp_for_frame(s->ctx, frame);
  const int64_t end_us = s->host->audio_timestamp_for_frame(s->ctx, frame + 1);
  const double exact =
      (static_cast<double>(end_us - start_us) * f.sample_rate * s->audio_clock / 1e6) + s->audio_remainder;
  const auto samples = static_cast<uint32_t>(exact);
  s->audio_remainder = exact - samples;
  if (samples == 0) return;
  s->tone_generator.generate(f, samples, s->tone.load(), &s->pcm);
  s->host->push_audio(s->ctx, s->pcm.data(), samples, start_us);
}

/** Audio has no gaps: every engine frame since the last one gets its samples, including frames without video. */
void push_audio_through(CaptureSim* s, uint32_t last_frame, uint32_t frame) {
  const uint32_t first = last_frame == 0 ? frame : last_frame + 1;
  for (uint32_t f = first; f <= frame; f++) push_frame_audio(s, f);
}

// ---------------------------------------------------------------------------
// Capture thread
// ---------------------------------------------------------------------------

void report_status(CaptureSim* s) {
  const auto frames = static_cast<unsigned long long>(s->frames.load());
  const auto dropped = static_cast<unsigned long long>(s->dropped.load());
  char text[96];
  std::snprintf(text, sizeof text, "frames=%llu dropped=%llu", frames, dropped);
  char json[128];
  std::snprintf(json, sizeof json, "{\"frames\":%llu,\"dropped\":%llu}", frames, dropped);
  s->host->set_status(s->ctx, SESAME_STATE_ONLINE, text, json);
}

void capture_loop(CaptureSim* s) {
  const int64_t frame_us =
      s->info.fps_num > 0 ? static_cast<int64_t>(1000000.0 * s->info.fps_den / s->info.fps_num) : 20000;
  uint32_t last_frame = 0;
  while (s->running.load()) {
    // Decide which engine frame the next captured frame belongs to. The engine renders `next_frame` soon, so the
    // frame after that leaves a full frame period for acquiring, filling and uploading the slot.
    uint32_t next_frame = 0;
    int64_t offset_us = 0;
    uint32_t frame = 0;
    if (s->host->get_frame_info(s->ctx, &next_frame, &offset_us)) {
      frame = next_frame + 2;
      if (frame <= last_frame) {
        // This engine frame already has its frame: wait for the next engine frame to start.
        std::this_thread::sleep_for(std::chrono::microseconds(std::max<int64_t>(frame_us - offset_us, 500)));
        continue;
      }
    } else {
      // No engine timing yet: run at the nominal frame rate.
      frame = last_frame + 1;
      std::this_thread::sleep_for(std::chrono::microseconds(frame_us));
    }

    // An interlaced frame holds two fields and covers two engine frames, so it is submitted on even frames only.
    if (s->interlaced.load() && (frame & 1) != 0) {
      push_audio_through(s, last_frame, frame);
      last_frame = frame;
      continue;
    }

    // Acquire a slot. It fails when every slot is still in use; a device would drop this frame.
    sesame_frame_slot* slot = nullptr;
    if (!s->host->acquire_frame(s->ctx, &slot)) {
      s->dropped++;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }

    // Fill it in the slot's own format, which is the format declared when the slot was acquired.
    s->bars.paint(slot, frame, s->speed.load(), static_cast<uint8_t>(s->alpha.load()));
    slot->frame = frame;
    slot->alpha_mode = SESAME_ALPHA_STRAIGHT;
    slot->timecode_frames = frame;
    slot->timecode_valid = 1;
    if (s->host->submit_frame(s->ctx, slot, SESAME_FRAME_FROM_HOST)) {
      if (++s->frames % STATUS_EVERY_FRAMES == 1) report_status(s);
    } else {
      s->dropped++;
    }

    push_audio_through(s, last_frame, frame);
    last_frame = frame;
  }
}

// ---------------------------------------------------------------------------
// Stream simulator
// ---------------------------------------------------------------------------

/** Audio for one stretch of the device's own time, stamped with that time. */
void push_device_audio(CaptureSim* s, int64_t device_us, double duration_us) {
  const sesame_audio_format& f = s->audio;
  if (f.channels == 0) return;
  const double exact = (duration_us * f.sample_rate / 1e6) + s->audio_remainder;
  const auto samples = static_cast<uint32_t>(exact);
  s->audio_remainder = exact - samples;
  if (samples == 0) return;
  s->tone_generator.generate(f, samples, s->tone.load(), &s->pcm);
  // A DEVICE-timed source passes device time; the host maps it to engine time.
  s->host->push_audio(s->ctx, s->pcm.data(), samples, device_us);
}

/**
 * Sends a frame every frame period of the device's own clock, which runs
 * clock_ppm off the engine's. Each frame arrives up to jitter_us after its
 * ideal time. The loop reads the engine clock only to simulate arrival times;
 * it never assigns engine frames.
 */
void stream_loop(CaptureSim* s) {
  const double frame_us = s->info.fps_num > 0 ? 1e6 * s->info.fps_den / s->info.fps_num : 20000.0;
  uint64_t index = 0;
  double start_engine_us = -1.0;
  int64_t next_arrival_us = -1;
  while (s->running.load()) {
    uint32_t next_frame = 0;
    int64_t offset_us = 0;
    if (!s->host->get_frame_info(s->ctx, &next_frame, &offset_us) || next_frame == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    const int64_t now_us = s->host->audio_timestamp_for_frame(s->ctx, next_frame - 1) + offset_us;
    if (start_engine_us < 0) start_engine_us = static_cast<double>(now_us);
    // An interlaced frame carries two fields: one frame per two engine frame periods.
    const bool interlaced = s->interlaced.load();
    const double device_frame_us = frame_us * (interlaced ? 2.0 : 1.0);
    const double device_us = static_cast<double>(index) * device_frame_us;
    if (next_arrival_us < 0) {
      std::uniform_int_distribution<int64_t> delay(0, s->jitter_us);
      const double ideal = start_engine_us + (device_us / (1.0 + (s->clock_ppm / 1e6)));
      next_arrival_us = static_cast<int64_t>(ideal) + (s->jitter_us > 0 ? delay(s->random) : 0);
    }
    if (now_us < next_arrival_us) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    sesame_frame_slot* slot = nullptr;
    if (!s->host->acquire_frame(s->ctx, &slot)) {
      s->dropped++;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    // The painter counts in engine frames; an interlaced frame's second field is one engine frame later.
    const auto pattern_frame = static_cast<uint32_t>(interlaced ? index * 2 : index);
    s->bars.paint(slot, pattern_frame, s->speed.load(), static_cast<uint8_t>(s->alpha.load()));
    slot->device_time_us = static_cast<int64_t>(device_us);
    slot->alpha_mode = SESAME_ALPHA_STRAIGHT;
    if (s->host->submit_frame(s->ctx, slot, SESAME_FRAME_FROM_HOST)) {
      if (++s->frames % STATUS_EVERY_FRAMES == 1) report_status(s);
    } else {
      s->dropped++;
    }
    push_device_audio(s, static_cast<int64_t>(device_us), device_frame_us);
    index++;
    next_arrival_us = -1;
  }
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

sesame_instance create(const sesame_host* host, void* ctx, const char* /*id*/, const sesame_engine_info* info,
                       const sesame_param* p, uint32_t n) {
  auto* s = new CaptureSim{};
  s->host = host;
  s->ctx = ctx;
  s->info = *info;
  s->speed = std::atoi(param(p, n, "speed", "4"));
  s->tone = param_bool(p, n, "tone");
  if (!apply_format(s, p, n)) {
    delete s;
    return nullptr;
  }
  return s;
}

sesame_instance create_stream(const sesame_host* host, void* ctx, const char* id, const sesame_engine_info* info,
                              const sesame_param* p, uint32_t n) {
  auto* s = static_cast<CaptureSim*>(create(host, ctx, id, info, p, n));
  if (s == nullptr) return nullptr;
  s->stream = true;
  s->clock_ppm = std::strtod(param(p, n, "clockPpm", "0"), nullptr);
  s->jitter_us = static_cast<int64_t>(std::strtod(param(p, n, "jitterMs", "0"), nullptr) * 1000.0);
  // The buffer this transport needs by default; the operator's config can override it.
  host->set_target_buffer(ctx, static_cast<uint32_t>(std::strtoul(param(p, n, "bufferFrames", "3"), nullptr, 10)));
  return s;
}

void destroy(sesame_instance self) { delete static_cast<CaptureSim*>(self); }

sesame_bool start(sesame_instance self) {
  auto* s = static_cast<CaptureSim*>(self);
  s->running = true;
  s->worker = s->stream ? std::thread(stream_loop, s) : std::thread(capture_loop, s);
  return 1;
}

void stop(sesame_instance self) {
  auto* s = static_cast<CaptureSim*>(self);
  s->running = false;
  if (s->worker.joinable()) s->worker.join();
}

// Every parameter can change while running: a format change is declared again, as on a new input signal.
sesame_bool can_update(sesame_instance, const sesame_param*, uint32_t) { return 1; }

sesame_bool update(sesame_instance self, const sesame_param* p, uint32_t n) {
  auto* s = static_cast<CaptureSim*>(self);
  if (!apply_format(s, p, n)) return 0;
  s->speed = std::atoi(param(p, n, "speed", "4"));
  s->tone = param_bool(p, n, "tone");
  return 1;
}

// The engine calls this on its timer thread when the instance is used as clock; it must not block.
sesame_bool get_clock(sesame_instance, sesame_clock_time* time) {
  using namespace std::chrono;
  time->now_us = static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
  time->start_us = 0;
  return 1;
}

// ---------------------------------------------------------------------------
// Descriptor, vtable and entry
// ---------------------------------------------------------------------------

const sesame_plugin_descriptor DESCRIPTOR = {
    .struct_size = sizeof(sesame_plugin_descriptor),
    .id = "com.example.capture-sim",
    .name = "Capture Simulator",
    .version = "1.0.0",
    .kind = SESAME_PLUGIN_SOURCE,
    .has_video = 1,
    .max_audio_channels = 2,
    .params = PARAMS,
    .param_count = std::size(PARAMS),
    .source_mode = SESAME_SOURCE_PUSH,
    .provides_clock = 1,
    .ring_depth = 4,
};

const sesame_source_vtable VTABLE = {
    .struct_size = sizeof(sesame_source_vtable),
    .create = create,
    .destroy = destroy,
    .start = start,
    .stop = stop,
    .can_update = can_update,
    .update = update,
    .get_clock = get_clock,
};

const sesame_plugin_descriptor STREAM_DESCRIPTOR = {
    .struct_size = sizeof(sesame_plugin_descriptor),
    .id = "com.example.stream-sim",
    .name = "Stream Simulator",
    .version = "1.0.0",
    .kind = SESAME_PLUGIN_SOURCE,
    .has_video = 1,
    .max_audio_channels = 2,
    .params = STREAM_PARAMS,
    .param_count = std::size(STREAM_PARAMS),
    .source_mode = SESAME_SOURCE_PUSH,
    .source_timing = SESAME_TIMING_DEVICE,
};

const sesame_source_vtable STREAM_VTABLE = {
    .struct_size = sizeof(sesame_source_vtable),
    .create = create_stream,
    .destroy = destroy,
    .start = start,
    .stop = stop,
    .can_update = can_update,
    .update = update,
};

struct Type {
  const sesame_plugin_descriptor* descriptor;
  const sesame_source_vtable* vtable;
};

const Type TYPES[] = {
    {&DESCRIPTOR, &VTABLE},
    {&STREAM_DESCRIPTOR, &STREAM_VTABLE},
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
