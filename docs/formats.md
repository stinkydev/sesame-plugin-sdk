# Formats

The byte layout of every pixel and sample format a plugin can exchange with
Sesame. Sources can deliver any of the pixel formats, and outputs can ask for
any of them.

## Pixel formats

Conventions for all of them:

- `w` and `h` are the frame's width and height, and `p` is `pitch_bytes`, the
  row pitch of the first plane. Minimum pitches are listed per format; a source
  may use a larger pitch (a multiple of 4) to match padded hardware rows, and
  outputs always receive the minimum.
- Planes follow each other with no gap: the second plane starts at `p * h`, and
  so on.
- 16-bit samples are little-endian. So are the 32-bit words of v210.
- YUV formats and R210 carry a matrix (BT.709, BT.601, BT.2020) and a range.
  Limited-range 8-bit codes are 16 to 235 for luma and 16 to 240 for chroma,
  with 10-bit and 16-bit codes scaled up (64 to 940 in 10 bits). 8-bit RGB is
  always full range. Output is always BT.709 limited range.
- 4:2:2 formats share one chroma sample between two horizontal pixels, and
  4:2:0 formats between a 2x2 block. They need an even width, and 4:2:0 an even
  height too.

| Format | Kind | Minimum pitch | Planes and total size |
|---|---|---|---|
| `RGBA8` | 8-bit RGB with alpha | `w * 4` | one: `p * h` |
| `BGRA8` | 8-bit RGB with alpha | `w * 4` | one: `p * h` |
| `R210` | 10-bit RGB | 256 bytes per 64 pixels | one: `p * h` |
| `UYVY` | 8-bit 4:2:2 | `w * 2` | one: `p * h` |
| `YUYV` | 8-bit 4:2:2 | `w * 2` | one: `p * h` |
| `UYVA` | 8-bit 4:2:2 with key | `w * 2` | UYVY, then key: `p * h + (p / 2) * h` |
| `V210` | 10-bit 4:2:2 | 128 bytes per 48 pixels | one: `p * h` |
| `RFC4175_422_10` | 10-bit 4:2:2 | `(w / 2) * 5` | one: `p * h` |
| `P216` | 16-bit 4:2:2 | `w * 2` | luma, chroma: `p * h * 2` |
| `PA16` | 16-bit 4:2:2 with key | `w * 2` | luma, chroma, key: `p * h * 3` |
| `NV12` | 8-bit 4:2:0 | `w` | luma, chroma: `p * h * 3 / 2` |
| `P010` | 10-bit 4:2:0 | `w * 2` | luma, chroma: `p * h * 3 / 2` |
| `I420` | 8-bit 4:2:0 | `w` | luma, Cb, Cr: `p * h + 2 * (p / 2) * (h / 2)` |

Typical sources: UYVY, v210 and R210 from SDI cards (Blackmagic, AJA, Deltacast);
RFC 4175 from SMPTE ST 2110-20 receivers with the RTP headers removed; UYVY,
UYVA, P216, PA16, NV12, I420, BGRA and RGBA from NDI; YUYV from USB and UVC
devices; NV12, P010 and I420 from hardware and software decoders.

### RGBA8 and BGRA8

Four bytes per pixel: R, G, B, A for RGBA8, and B, G, R, A for BGRA8. Alpha is
straight unless a source sets `SESAME_ALPHA_PREMULTIPLIED` on its slot.

### R210

One big-endian 32-bit word per pixel: two unused bits, then 10 bits each of R, G
and B.

```
bit 31 30 | 29 ........ 20 | 19 ........ 10 | 9 ......... 0
    unused |       R        |       G        |       B
```

The codes use the frame's range: limited is 64 to 940, full is 0 to 1023.

### UYVY and YUYV

Two pixels in four bytes, sharing one Cb and Cr:

```
UYVY:  Cb0  Y0  Cr0  Y1
YUYV:  Y0  Cb0  Y1  Cr0
```

### UYVA

A UYVY plane, then a key plane: one byte per pixel, `h` rows at pitch `p / 2`.
255 is opaque. This is NDI's UYVA layout.

### V210

Six pixels in 16 bytes, as four little-endian 32-bit words of three 10-bit
components each (bits 0-9, 10-19 and 20-29; bits 30-31 unused):

| Word | Bits 0-9 | Bits 10-19 | Bits 20-29 |
|---|---|---|---|
| 0 | Cb0 | Y0 | Cr0 |
| 1 | Y1 | Cb2 | Y2 |
| 2 | Cr2 | Y3 | Cb4 |
| 3 | Y4 | Cr4 | Y5 |

Pixels 0 and 1 share Cb0 and Cr0, pixels 2 and 3 share Cb2 and Cr2, and pixels
4 and 5 share Cb4 and Cr4. Rows are padded to a multiple of 48 pixels
(128 bytes), as SDI hardware delivers them; a width that is not a multiple of 6
fills the last group by repeating the last pixel.

### RFC4175_422_10

The 10-bit 4:2:2 pixel group of RFC 4175 and SMPTE ST 2110-20: two pixels in
five bytes, 40 bits most significant first.

```
byte   0        1        2        3        4
bits   Cb (10)    | Y0 (10)   | Cr (10)   | Y1 (10)
```

Rows are tightly packed: the RTP payload of a line, with the RTP and payload
headers removed and the lines laid out one after another.

### P216 and PA16

A luma plane of 16-bit samples (`w * 2` bytes per row), then a chroma plane of
the same size at full height with Cb and Cr interleaved (one Cb, Cr pair of
16-bit samples per two pixels). PA16 adds a third plane of 16-bit key samples,
one per pixel; Sesame uses the high byte of each, and writes the key as
`alpha * 257`.

### NV12 and P010

A luma plane, then a chroma plane at half height with Cb and Cr interleaved,
one pair per 2x2 block. NV12 samples are 8-bit. P010 samples are 16-bit with
the 10-bit value in the high bits (the low 6 bits zero).

### I420

A luma plane, then a Cb plane and a Cr plane, each at half width and half height
with pitch `p / 2`.

### Interlaced frames

An interlaced frame holds two fields: the even rows (0, 2, 4, ...) are the top
field, and the odd rows the bottom field. `field_order` says which one comes
first in time.

By default the fields are interleaved row by row. With `field_sequential` set,
the rows of the first field in time are stored first and the second field's
rows follow: frame row `y` is stored at row `y / 2` if it belongs to the first
field, and at row `h / 2 + y / 2` otherwise. Every plane is stored the same way.
Field-sequential storage and interlaced output are not available for 4:2:0
formats, because their chroma rows are shared between the two fields.

## Audio

| Format | Sample |
|---|---|
| `S16` | 16-bit signed, little-endian |
| `S24` | 24-bit signed, little-endian, packed in 3 bytes |
| `S32` | 32-bit signed, little-endian |
| `F32` | 32-bit float, full scale at +-1.0 |
| `L16` | 16-bit signed, big-endian, as AES67 and SMPTE ST 2110-30 carry it |
| `L24` | 24-bit signed, big-endian, packed in 3 bytes, as AES67 and SMPTE ST 2110-30 carry it |

Audio is interleaved (sample 0 of every channel, then sample 1 of every
channel, and so on) or planar (all of channel 0, then all of channel 1, and so
on). Rates from 8 to 192 kHz are accepted. Sesame mixes 32-bit float at 48 kHz
and converts and resamples at both ends.
