/* Copyright (c) 2022-2026 Stinky Computing AB. SPDX-License-Identifier: MIT (see LICENSE in the SDK folder) */

#include "test-pattern.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace capture_sim {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double TONE_HZ = 1000.0;
constexpr double TONE_AMPLITUDE = 0.1;  // -20 dBFS

using Rgb = BarPainter::Rgb;
using Yuv = BarPainter::Yuv;

// Bar `band` (0..7) is black, red, green, yellow, blue, magenta, cyan or white, at 0x20 and 0xff.
Rgb bar_color(uint32_t x, uint32_t width, uint32_t frame, int speed) {
  const uint32_t bar = width / 8 == 0 ? 1 : width / 8;
  const uint32_t shift = (frame * static_cast<uint32_t>(speed)) % width;
  const uint32_t band = ((x + shift) / bar) & 7;
  return Rgb{static_cast<uint8_t>((band & 1) != 0 ? 0xff : 0x20), static_cast<uint8_t>((band & 2) != 0 ? 0xff : 0x20),
             static_cast<uint8_t>((band & 4) != 0 ? 0xff : 0x20)};
}

// BT.709 limited range at 8, 10 or 16 bits.
Yuv to_yuv(Rgb c, int bits) {
  const double r = c.r / 255.0;
  const double g = c.g / 255.0;
  const double b = c.b / 255.0;
  const double y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  const double cb = (b - y) / 1.8556;
  const double cr = (r - y) / 1.5748;
  const double step = static_cast<double>(1 << (bits - 8));
  return Yuv{static_cast<uint32_t>(std::lround((16 + 219 * y) * step)),
             static_cast<uint32_t>(std::lround((128 + 224 * cb) * step)),
             static_cast<uint32_t>(std::lround((128 + 224 * cr) * step))};
}

int bits_of(sesame_pixel_format format) {
  switch (format) {
    case SESAME_PIXEL_V210:
    case SESAME_PIXEL_P010:
    case SESAME_PIXEL_RFC4175_422_10:
      return 10;
    case SESAME_PIXEL_P216:
    case SESAME_PIXEL_PA16:
      return 16;
    default:
      return 8;
  }
}

void put16(uint8_t* p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v & 0xff);
  p[1] = static_cast<uint8_t>(v >> 8);
}

// Where frame row y is stored: interleaved, or each field's rows together, first field first.
uint32_t stored_row(const sesame_frame_slot* slot, uint32_t y) {
  if (slot->field_sequential == 0) return y;
  const uint32_t first_parity = slot->field_order == SESAME_FIELD_BOTTOM_FIRST ? 1 : 0;
  return (y & 1) == first_parity ? y / 2 : (slot->height / 2) + (y / 2);
}

bool is_second_field(const sesame_frame_slot* slot, uint32_t y) {
  if (slot->field_order == SESAME_FIELD_PROGRESSIVE) return false;
  const uint32_t first_parity = slot->field_order == SESAME_FIELD_BOTTOM_FIRST ? 1 : 0;
  return (y & 1) != first_parity;
}

// One sample; v is in [-1, 1].
void put_sample(uint8_t* p, double v, sesame_sample_format format) {
  switch (format) {
    case SESAME_SAMPLE_S16: {
      const auto x = static_cast<int16_t>(std::lround(v * 32767.0));
      std::memcpy(p, &x, 2);
      break;
    }
    case SESAME_SAMPLE_L16: {
      const auto x = static_cast<uint16_t>(static_cast<int16_t>(std::lround(v * 32767.0)));
      p[0] = static_cast<uint8_t>(x >> 8);
      p[1] = static_cast<uint8_t>(x);
      break;
    }
    case SESAME_SAMPLE_S24:
    case SESAME_SAMPLE_L24: {
      const auto x = static_cast<uint32_t>(static_cast<int32_t>(std::lround(v * 8388607.0)));
      const bool big = format == SESAME_SAMPLE_L24;
      p[big ? 2 : 0] = static_cast<uint8_t>(x);
      p[1] = static_cast<uint8_t>(x >> 8);
      p[big ? 0 : 2] = static_cast<uint8_t>(x >> 16);
      break;
    }
    case SESAME_SAMPLE_S32: {
      const auto x = static_cast<int32_t>(std::llround(v * 2147483647.0));
      std::memcpy(p, &x, 4);
      break;
    }
    case SESAME_SAMPLE_F32: {
      const auto x = static_cast<float>(v);
      std::memcpy(p, &x, 4);
      break;
    }
  }
}

}  // namespace

size_t sample_width(sesame_sample_format format) {
  switch (format) {
    case SESAME_SAMPLE_S24:
    case SESAME_SAMPLE_L24:
      return 3;
    case SESAME_SAMPLE_S32:
    case SESAME_SAMPLE_F32:
      return 4;
    default:
      return 2;
  }
}

void BarPainter::paint(sesame_frame_slot* slot, uint32_t frame, int speed, uint8_t alpha) {
  const uint32_t w = slot->width;
  const uint32_t h = slot->height;
  const size_t pitch = slot->pitch_bytes;
  uint8_t* base = slot->host_data;
  // The bars only change along x, so each field's colors are worked out once per frame.
  const int bits = bits_of(slot->pixel_format);
  for (int field = 0; field < 2; field++) {
    rgb_[field].resize(w);
    yuv_[field].resize(w);
    for (uint32_t x = 0; x < w; x++) {
      rgb_[field][x] = bar_color(x, w, frame + static_cast<uint32_t>(field), speed);
      yuv_[field][x] = to_yuv(rgb_[field][x], bits);
    }
  }
  for (uint32_t y = 0; y < h; y++) {
    const int field = is_second_field(slot, y) ? 1 : 0;
    const uint32_t sy = stored_row(slot, y);
    uint8_t* row = base + (sy * pitch);
    const auto& rgb = rgb_[field];
    const auto& yuv = yuv_[field];
    auto color = [&](uint32_t x) { return rgb[std::min(x, w - 1)]; };
    auto ycc = [&](uint32_t x) { return yuv[std::min(x, w - 1)]; };
    switch (slot->pixel_format) {
      case SESAME_PIXEL_RGBA8:
      case SESAME_PIXEL_BGRA8: {
        const bool bgra = slot->pixel_format == SESAME_PIXEL_BGRA8;
        for (uint32_t x = 0; x < w; x++) {
          const Rgb c = color(x);
          uint8_t* px = row + (x * 4);
          px[0] = bgra ? c.b : c.r;
          px[1] = c.g;
          px[2] = bgra ? c.r : c.b;
          px[3] = alpha;
        }
        break;
      }
      case SESAME_PIXEL_R210:
        for (uint32_t x = 0; x < w; x++) {
          const Rgb c = color(x);
          auto code = [](uint8_t v) { return static_cast<uint32_t>(std::lround(64 + (876 * (v / 255.0)))); };
          const uint32_t word = (code(c.r) << 20) | (code(c.g) << 10) | code(c.b);
          uint8_t* px = row + (x * 4);
          px[0] = static_cast<uint8_t>(word >> 24);
          px[1] = static_cast<uint8_t>(word >> 16);
          px[2] = static_cast<uint8_t>(word >> 8);
          px[3] = static_cast<uint8_t>(word);
        }
        break;
      case SESAME_PIXEL_UYVY:
      case SESAME_PIXEL_YUYV:
      case SESAME_PIXEL_UYVA: {
        const bool yuyv = slot->pixel_format == SESAME_PIXEL_YUYV;
        for (uint32_t x = 0; x < w; x += 2) {
          const Yuv a = ycc(x);
          const Yuv b = ycc(x + 1);
          uint8_t* px = row + (x * 2);
          const auto cb = static_cast<uint8_t>((a.cb + b.cb) / 2);
          const auto cr = static_cast<uint8_t>((a.cr + b.cr) / 2);
          px[0] = yuyv ? static_cast<uint8_t>(a.y) : cb;
          px[1] = yuyv ? cb : static_cast<uint8_t>(a.y);
          px[2] = yuyv ? static_cast<uint8_t>(b.y) : cr;
          px[3] = yuyv ? cr : static_cast<uint8_t>(b.y);
        }
        if (slot->pixel_format == SESAME_PIXEL_UYVA) {
          std::memset(base + (pitch * h) + (sy * (pitch / 2)), alpha, w);
        }
        break;
      }
      case SESAME_PIXEL_V210: {
        auto* words = reinterpret_cast<uint32_t*>(row);
        for (uint32_t x = 0; x < w; x += 6, words += 4) {
          Yuv p[6];
          for (uint32_t i = 0; i < 6; i++) p[i] = ycc(x + i);
          words[0] = p[0].cb | (p[0].y << 10) | (p[0].cr << 20);
          words[1] = p[1].y | (p[2].cb << 10) | (p[2].y << 20);
          words[2] = p[2].cr | (p[3].y << 10) | (p[4].cb << 20);
          words[3] = p[4].y | (p[4].cr << 10) | (p[5].y << 20);
        }
        break;
      }
      case SESAME_PIXEL_RFC4175_422_10:
        for (uint32_t x = 0; x < w; x += 2) {
          const Yuv a = ycc(x);
          const Yuv b = ycc(x + 1);
          const uint64_t pgroup = (uint64_t{(a.cb + b.cb) / 2} << 30) | (uint64_t{a.y} << 20) |
                                  (uint64_t{(a.cr + b.cr) / 2} << 10) | uint64_t{b.y};
          uint8_t* g = row + ((x / 2) * 5);
          for (int i = 0; i < 5; i++) g[i] = static_cast<uint8_t>(pgroup >> (32 - (8 * i)));
        }
        break;
      case SESAME_PIXEL_P216:
      case SESAME_PIXEL_PA16:
        for (uint32_t x = 0; x < w; x++) {
          const Yuv c = ycc(x);
          put16(row + (x * 2), c.y);
          if ((x & 1) == 0) {
            uint8_t* uv = base + (pitch * h) + (sy * pitch) + (x * 2);
            put16(uv, c.cb);
            put16(uv + 2, c.cr);
          }
          if (slot->pixel_format == SESAME_PIXEL_PA16) {
            put16(base + (pitch * h * 2) + (sy * pitch) + (x * 2), alpha * 257u);
          }
        }
        break;
      case SESAME_PIXEL_NV12:
      case SESAME_PIXEL_P010:
      case SESAME_PIXEL_I420: {
        const bool p010 = slot->pixel_format == SESAME_PIXEL_P010;
        for (uint32_t x = 0; x < w; x++) {
          const Yuv c = ycc(x);
          if (p010) {
            put16(row + (x * 2), c.y << 6);
          } else {
            row[x] = static_cast<uint8_t>(c.y);
          }
          if ((x & 1) != 0 || (y & 1) != 0) continue;
          if (slot->pixel_format == SESAME_PIXEL_I420) {
            const size_t half = pitch / 2;
            uint8_t* cb = base + (pitch * h) + ((y / 2) * half) + (x / 2);
            cb[0] = static_cast<uint8_t>(c.cb);
            cb[half * (h / 2)] = static_cast<uint8_t>(c.cr);
          } else {
            uint8_t* uv = base + (pitch * h) + ((y / 2) * pitch) + (p010 ? x * 2 : x);
            if (p010) {
              put16(uv, c.cb << 6);
              put16(uv + 2, c.cr << 6);
            } else {
              uv[0] = static_cast<uint8_t>(c.cb);
              uv[1] = static_cast<uint8_t>(c.cr);
            }
          }
        }
        break;
      }
    }
  }
}

void ToneGenerator::generate(const sesame_audio_format& format, uint32_t samples, bool on, std::vector<uint8_t>* out) {
  const size_t width = sample_width(format.format);
  out->resize(static_cast<size_t>(samples) * format.channels * width);
  const double step = 2.0 * PI * TONE_HZ / format.sample_rate;
  for (uint32_t i = 0; i < samples; i++, phase_ += step) {
    const double v = on ? TONE_AMPLITUDE * std::sin(phase_) : 0.0;
    for (uint32_t c = 0; c < format.channels; c++) {
      // Planar: each channel's samples together. Interleaved: one sample of every channel at a time.
      const size_t index = format.planar != 0 ? (static_cast<size_t>(c) * samples) + i
                                              : (static_cast<size_t>(i) * format.channels) + c;
      put_sample(out->data() + (index * width), v, format.format);
    }
  }
  phase_ = std::fmod(phase_, 2.0 * PI);
}

}  // namespace capture_sim
