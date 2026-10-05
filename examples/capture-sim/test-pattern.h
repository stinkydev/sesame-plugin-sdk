/* Copyright (c) 2022-2026 Stinky Computing AB. SPDX-License-Identifier: MIT (see LICENSE in the SDK folder) */

// The capture simulator's test signal: moving color bars in every pixel
// format, and a sine tone in every sample format. A capture plugin receives
// these bytes from its hardware; the simulator generates them.

#pragma once

#include <sesame-plugin.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace capture_sim {

/** Bytes per sample of a sample format. */
size_t sample_width(sesame_sample_format format);

/**
 * Eight vertical color bars that move `speed` pixels per engine frame. Paints
 * into a slot in the slot's own size, pixel format, pitch and scan, as a card
 * delivers its native signal.
 */
class BarPainter {
 public:
  /**
   * `frame` is the engine frame the slot shows; in an interlaced slot the
   * second field shows the bars one engine frame later. `alpha` is written to
   * formats with alpha or a key.
   */
  void paint(sesame_frame_slot* slot, uint32_t frame, int speed, uint8_t alpha);

  struct Rgb {
    uint8_t r, g, b;
  };
  struct Yuv {
    uint32_t y, cb, cr;  // BT.709 limited range codes at the format's bit depth
  };

 private:
  // One row of colors per field, reused every frame.
  std::vector<Rgb> rgb_[2];
  std::vector<Yuv> yuv_[2];
};

/** A 1 kHz sine at -20 dBFS on every channel, continuous across calls. */
class ToneGenerator {
 public:
  /** `samples` samples per channel in `format` into `out`; silence when `on` is false. */
  void generate(const sesame_audio_format& format, uint32_t samples, bool on, std::vector<uint8_t>* out);

 private:
  double phase_ = 0.0;
};

}  // namespace capture_sim
