/* Copyright (c) 2022-2026 Stinky Computing AB. SPDX-License-Identifier: MIT (see LICENSE in the SDK folder) */

// Colour generator: a PULL source that fills its frame with one colour. It
// demonstrates:
//
//  - produce(), called by the host once per engine frame on its worker thread;
//  - returning SESAME_FRAME_NONE while nothing changed, which keeps the
//    previous frame and costs no upload;
//  - a colour parameter changed at runtime through update();
//  - metadata: the current colour is published as state on
//    com.example.color-generator.state.v1, and a JSON payload
//    {"color":"#rrggbb"} on com.example.color-generator.set.v1 sets it.

#define SESAME_PLUGIN_IMPLEMENTATION
#include <sesame-plugin.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace {

constexpr const char* STATE_ID = "com.example.color-generator.state.v1";
constexpr const char* SET_ID = "com.example.color-generator.set.v1";

// ---------------------------------------------------------------------------
// Parameters and metadata
// ---------------------------------------------------------------------------

// key, label, type, default, min, max, enum values, enum count
const sesame_param_def PARAMS[] = {
    {"color", "Color", SESAME_PARAM_COLOR, "#000000ff", 0, 0, nullptr, 0},
};

// id, label, receive (0 = published by the plugin, 1 = received), format
const sesame_metadata_def METADATA[] = {
    {STATE_ID, "Colour state", 0, "json"},
    {SET_ID, "Set colour", 1, "json"},
};

const char* param(const sesame_param* p, uint32_t n, const char* key, const char* fallback) {
  for (uint32_t i = 0; i < n; i++) {
    if (std::strcmp(p[i].key, key) == 0) return p[i].value;
  }
  return fallback;
}

/** "#rrggbb" or "#rrggbbaa" as an RGBA8 pixel in a little-endian word: R in the lowest byte. */
uint32_t parse_color(const char* text) {
  constexpr uint32_t OPAQUE_BLACK = 0xff000000;
  if (text == nullptr || text[0] != '#') return OPAQUE_BLACK;
  const size_t digits = std::strlen(text + 1);
  if (digits != 6 && digits != 8) return OPAQUE_BLACK;
  const unsigned long value = std::strtoul(text + 1, nullptr, 16);
  const uint32_t rgba = digits == 6 ? (static_cast<uint32_t>(value) << 8) | 0xff : static_cast<uint32_t>(value);
  const uint32_t r = (rgba >> 24) & 0xff;
  const uint32_t g = (rgba >> 16) & 0xff;
  const uint32_t b = (rgba >> 8) & 0xff;
  const uint32_t a = rgba & 0xff;
  return r | (g << 8) | (b << 16) | (a << 24);
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

struct ColorGen {
  const sesame_host* host = nullptr;
  void* ctx = nullptr;
  // Written by update() and on_data(), read by produce().
  std::atomic<uint32_t> rgba{0xff000000};
  std::atomic<bool> changed{true};  // the next produce() writes a frame
};

/** Publishes the colour as state and shows it in the status. */
void publish_color(ColorGen* g) {
  const uint32_t c = g->rgba.load();
  char json[64];
  const int len = std::snprintf(json, sizeof json, "{\"color\":\"#%02x%02x%02x%02x\"}", c & 0xff, (c >> 8) & 0xff,
                                (c >> 16) & 0xff, (c >> 24) & 0xff);
  g->host->publish_metadata(g->ctx, STATE_ID, SESAME_DATA_JSON, json, static_cast<size_t>(len), SESAME_PUBLISH_STATE);
  g->host->set_status(g->ctx, SESAME_STATE_ONLINE, nullptr, json);
}

// ---------------------------------------------------------------------------
// Producing frames
// ---------------------------------------------------------------------------

// The host calls this once per engine frame, two frames ahead of rendering. The slot is RGBA8 at the render
// size, the default layout of a source that does not call set_video_format.
uint32_t produce(sesame_instance self, sesame_frame_slot* slot) {
  auto* g = static_cast<ColorGen*>(self);
  if (!g->changed.exchange(false)) return SESAME_FRAME_NONE;
  const uint32_t color = g->rgba.load();
  uint8_t* row = slot->host_data;
  for (uint32_t y = 0; y < slot->height; y++, row += slot->pitch_bytes) {
    auto* px = reinterpret_cast<uint32_t*>(row);
    for (uint32_t x = 0; x < slot->width; x++) px[x] = color;
  }
  slot->alpha_mode = SESAME_ALPHA_STRAIGHT;
  publish_color(g);
  return SESAME_FRAME_FROM_HOST;
}

// ---------------------------------------------------------------------------
// Control
// ---------------------------------------------------------------------------

// For a PULL source the host calls this on the worker thread, between produce() calls. The payload is parsed by
// hand to keep the example free of dependencies; a plugin would use a JSON library.
void on_data(sesame_instance self, const sesame_data* d) {
  auto* g = static_cast<ColorGen*>(self);
  if (d->metadata_id == nullptr || std::strcmp(d->metadata_id, SET_ID) != 0) return;
  const std::string payload(static_cast<const char*>(d->payload), d->len);
  const auto hash = payload.find('#');
  if (hash == std::string::npos) return;
  const auto end = payload.find('"', hash);
  g->rgba = parse_color(payload.substr(hash, end == std::string::npos ? std::string::npos : end - hash).c_str());
  g->changed = true;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

sesame_instance create(const sesame_host* host, void* ctx, const char* /*id*/, const sesame_engine_info* /*info*/,
                       const sesame_param* p, uint32_t n) {
  auto* g = new ColorGen{};
  g->host = host;
  g->ctx = ctx;
  g->rgba = parse_color(param(p, n, "color", "#000000ff"));
  return g;
}

void destroy(sesame_instance self) { delete static_cast<ColorGen*>(self); }

// The host runs produce() on its own worker, so there are no threads to start or stop.
sesame_bool start(sesame_instance) { return 1; }
void stop(sesame_instance) {}

sesame_bool can_update(sesame_instance, const sesame_param*, uint32_t) { return 1; }

sesame_bool update(sesame_instance self, const sesame_param* p, uint32_t n) {
  auto* g = static_cast<ColorGen*>(self);
  g->rgba = parse_color(param(p, n, "color", "#000000ff"));
  g->changed = true;
  return 1;
}

// ---------------------------------------------------------------------------
// Descriptor, vtable and entry
// ---------------------------------------------------------------------------

const sesame_plugin_descriptor DESCRIPTOR = {
    .struct_size = sizeof(sesame_plugin_descriptor),
    .id = "com.example.color-generator",
    .name = "Color Generator",
    .version = "1.0.0",
    .kind = SESAME_PLUGIN_SOURCE,
    .has_video = 1,
    .max_audio_channels = 0,
    .params = PARAMS,
    .param_count = std::size(PARAMS),
    .metadata = METADATA,
    .metadata_count = std::size(METADATA),
    .source_mode = SESAME_SOURCE_PULL,
};

const sesame_source_vtable VTABLE = {
    .struct_size = sizeof(sesame_source_vtable),
    .create = create,
    .destroy = destroy,
    .start = start,
    .stop = stop,
    .can_update = can_update,
    .update = update,
    .on_data = on_data,
    .produce = produce,
};

sesame_bool plugin_init(const char* /*library_path*/) { return 1; }
void plugin_deinit() {}
uint32_t descriptor_count() { return 1; }
const sesame_plugin_descriptor* descriptor(uint32_t index) { return index == 0 ? &DESCRIPTOR : nullptr; }
const void* vtable(const char* id) { return std::strcmp(id, DESCRIPTOR.id) == 0 ? &VTABLE : nullptr; }

}  // namespace

extern "C" SESAME_EXPORT const sesame_plugin_entry sesame_plugin_entry_v1 = {
    SESAME_PLUGIN_ABI_MAJOR, SESAME_PLUGIN_ABI_MINOR, plugin_init, plugin_deinit, descriptor_count, descriptor, vtable,
};
