/* Copyright (c) 2022-2026 Stinky Computing AB. SPDX-License-Identifier: MIT (see LICENSE in the SDK folder) */

/*
 * Sesame native plugin ABI.
 *
 * A plugin is a shared library exporting `sesame_plugin_entry_v1`. It provides
 * source and/or output types that the engine instantiates from config. The
 * SDK's docs/guide.md explains how to write one; the essentials:
 *
 *  - Every struct starts with struct_size. Fields are only appended. Readers
 *    use min(struct_size, sizeof(own definition)) and treat missing tail
 *    fields as zero.
 *  - Pointers passed into a plugin callback are borrowed for that call only,
 *    except the sesame_host pointer and ctx, which live until destroy returns.
 *  - Pointers the plugin returns (descriptors, vtables, param defs, strings)
 *    must stay valid from init until deinit.
 *  - No plugin code runs on the render thread. Sources fill frame slots from a
 *    host worker (PULL) or their own threads (PUSH); outputs receive video,
 *    audio and data on the output's own thread.
 *  - The host never calls destroy while a callback is in flight and never
 *    calls a callback after stop returns.
 *
 * Sources deliver frames in their own size and pixel format (set_video_format);
 * the host converts to the engine's RGBA and scales to the source's texture off
 * the render thread. Two modes:
 *
 *  - PULL: the host runs a worker thread and calls produce() once per engine
 *    frame, two frames ahead of rendering, with a slot already acquired. The
 *    plugin writes pixels (and audio via write_audio) and returns which
 *    memory it wrote. Missing the deadline repeats the previous frame.
 *  - PUSH: the plugin runs its own threads, calls acquire_frame, fills the
 *    slot and calls submit_frame. With ENGINE timing it sets slot->frame to
 *    the engine frame it belongs to, from get_frame_info and
 *    audio_timestamp_for_frame. With DEVICE timing it stamps each frame and
 *    audio block with its own clock, and the host measures that clock against
 *    the engine's, buffers by the operator's setting and places the frames,
 *    dropping or repeating evenly as the clocks drift.
 *
 * A source with provides_clock implements get_clock; when its config says
 * use_as_clock the engine paces rendering from it.
 *
 * The ENCODED source mode is reserved for a later ABI minor and is rejected by
 * this host. Outputs choose a video delivery: none, device memory, host memory
 * (both in a chosen pixel format), or ENCODED packets from a shared encoder;
 * audio arrives as PCM per mix or as one Opus stream.
 *
 * Audio crosses in the plugin's own sample format, layout and rate; the host
 * converts to and from the engine's 48 kHz float and resamples as needed.
 */

#ifndef SESAME_PLUGIN_H
#define SESAME_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32)
#define SESAME_EXPORT __declspec(dllexport)
#else
#define SESAME_EXPORT __attribute__((visibility("default")))
#endif

#define SESAME_PLUGIN_ABI_MAJOR 1
#define SESAME_PLUGIN_ABI_MINOR 0

typedef void* sesame_instance;    /* plugin-owned, opaque to the host */
typedef int32_t sesame_bool;      /* 0 or 1 */
typedef void* sesame_cuda_stream; /* CUstream */

typedef enum { SESAME_PLUGIN_SOURCE = 1, SESAME_PLUGIN_OUTPUT = 2 } sesame_plugin_kind;

typedef enum {
  SESAME_SOURCE_PULL = 0,   /* host worker calls produce() once per frame, two frames ahead */
  SESAME_SOURCE_PUSH = 1,   /* plugin threads acquire, fill and submit slots themselves */
  SESAME_SOURCE_ENCODED = 2 /* reserved: not accepted by ABI 1.0 hosts */
} sesame_source_mode;

typedef enum {
  SESAME_TIMING_ENGINE = 0, /* the plugin places frames on engine frames itself */
  SESAME_TIMING_DEVICE = 1  /* PUSH sources: the plugin stamps its own clock; the host syncs it to the engine */
} sesame_source_timing;

/* Returned by produce() and passed to submit_frame(): which memory of the slot holds the frame. */
#define SESAME_FRAME_NONE 0u        /* nothing produced; the previous frame stays on screen */
#define SESAME_FRAME_FROM_HOST 1u   /* host_data was written; the host uploads it */
#define SESAME_FRAME_FROM_DEVICE 2u /* device_data was written on the slot's stream */

#define SESAME_SOURCE_RING_MAX 16u
#define SESAME_VIDEO_MAX_DIMENSION 8192u

/*
 * Pixel layouts. Planes follow each other in the order listed, each starting
 * right after the previous one. "pitch" is the row pitch of the first plane;
 * planes marked "half pitch" use pitch / 2. Minimum pitches are tight packing,
 * except V210 (rows padded to 128 bytes per 48 pixels) and R210 (256 bytes per
 * 64 pixels), as SDI hardware delivers them. 16-bit samples are little-endian.
 */
typedef enum {
  SESAME_PIXEL_RGBA8 = 0, /* bytes R, G, B, A */
  SESAME_PIXEL_BGRA8 = 1, /* bytes B, G, R, A */
  SESAME_PIXEL_UYVY = 2,  /* 8-bit 4:2:2, bytes U0 Y0 V0 Y1 */
  SESAME_PIXEL_V210 = 3,  /* 10-bit 4:2:2, three components per little-endian 32-bit word */
  SESAME_PIXEL_NV12 = 4,  /* 8-bit 4:2:0: Y plane, interleaved CbCr plane at half height */
  SESAME_PIXEL_P010 = 5,  /* 10-bit 4:2:0 in the high bits of 16-bit samples, NV12 layout */
  SESAME_PIXEL_YUYV = 6,  /* 8-bit 4:2:2, bytes Y0 U0 Y1 V0 (YUY2) */
  SESAME_PIXEL_UYVA = 7,  /* UYVY plane, then an 8-bit alpha plane at half pitch (NDI UYVA) */
  SESAME_PIXEL_P216 = 8,  /* 16-bit 4:2:2: Y plane, interleaved CbCr plane at full height */
  SESAME_PIXEL_PA16 = 9,  /* P216 planes, then a 16-bit alpha plane at the same pitch */
  SESAME_PIXEL_I420 = 10, /* 8-bit 4:2:0: Y plane, Cb plane and Cr plane at half pitch and half height */
  SESAME_PIXEL_R210 = 11, /* 10-bit RGB, one big-endian 32-bit word per pixel: 2 unused bits, R, G, B */
  /* 10-bit 4:2:2 as in RFC 4175 and SMPTE ST 2110-20: two pixels per 5-byte pgroup, bits MSB first
     Cb0 Y0 Cr0 Y1, rows tightly packed (the RTP payload with its headers removed) */
  SESAME_PIXEL_RFC4175_422_10 = 12
} sesame_pixel_format;

/* Matrix: YUV formats only. Range: YUV formats and R210; 8-bit RGB formats are always full range. */
typedef enum { SESAME_MATRIX_BT709 = 0, SESAME_MATRIX_BT601 = 1, SESAME_MATRIX_BT2020 = 2 } sesame_color_matrix;
typedef enum { SESAME_RANGE_LIMITED = 0, SESAME_RANGE_FULL = 1 } sesame_color_range;

/*
 * Interlaced frames carry both fields; the host shows the first field on the
 * slot's engine frame and the second on the next one, each line-doubled.
 */
typedef enum {
  SESAME_FIELD_PROGRESSIVE = 0,
  SESAME_FIELD_TOP_FIRST = 1,   /* even rows are the first field in time */
  SESAME_FIELD_BOTTOM_FIRST = 2 /* odd rows are the first field in time */
} sesame_field_order;

/* Signalled so HDR sources can say what they carry; this host accepts SDR BT.709 only. */
typedef enum { SESAME_TRANSFER_SDR = 0, SESAME_TRANSFER_PQ = 1, SESAME_TRANSFER_HLG = 2 } sesame_transfer;
typedef enum { SESAME_PRIMARIES_BT709 = 0, SESAME_PRIMARIES_BT2020 = 1 } sesame_primaries;

typedef enum {
  SESAME_ALPHA_STRAIGHT = 0,     /* colour is not multiplied by alpha (an opaque frame is straight) */
  SESAME_ALPHA_PREMULTIPLIED = 1 /* fill already multiplied by key */
} sesame_alpha_mode;

typedef enum {
  SESAME_VIDEO_NONE = 0,    /* output receives no video */
  SESAME_VIDEO_DEVICE = 1,  /* on_video gets device memory in output_format */
  SESAME_VIDEO_HOST = 2,    /* on_video gets pinned host memory in output_format */
  SESAME_VIDEO_ENCODED = 3  /* on_encoded_video gets packets from the shared encoder named by encoder_id */
} sesame_video_delivery;

/* Sample formats. Multi-byte samples are little-endian except L16 and L24. */
typedef enum {
  SESAME_SAMPLE_S16 = 0, /* 16-bit signed */
  SESAME_SAMPLE_S24 = 1, /* 24-bit signed, packed in 3 bytes */
  SESAME_SAMPLE_S32 = 2, /* 32-bit signed */
  SESAME_SAMPLE_F32 = 3, /* 32-bit float, full scale at +-1.0 */
  SESAME_SAMPLE_L24 = 4, /* 24-bit signed big-endian, packed in 3 bytes (AES67, SMPTE ST 2110-30) */
  SESAME_SAMPLE_L16 = 5  /* 16-bit signed big-endian (AES67, SMPTE ST 2110-30) */
} sesame_sample_format;

/* Audio as a plugin delivers or receives it. Planar data stores the channels one after the other. */
typedef struct {
  uint32_t struct_size;
  uint32_t sample_rate; /* 8000 to 192000 */
  uint32_t channels;
  sesame_sample_format format;
  sesame_bool planar;
  /*
   * PUSH sources whose audio runs on a clock of its own, unrelated to any video
   * (AES67 and ST 2110-30 streams, NDI, USB or analog interfaces): the host
   * writes the audio back to back and resamples it gently to follow the pushed
   * timestamps. Leave it off for audio locked to video, such as SDI embedded
   * audio, and push each frame's samples at that frame's timestamp.
   */
  sesame_bool drift_compensation;
} sesame_audio_format;

typedef enum {
  SESAME_AUDIO_LAYOUT_PER_MIX = 0,     /* one on_audio per bound mix, each a stereo pair */
  SESAME_AUDIO_LAYOUT_MULTICHANNEL = 1 /* one on_audio with every bound mix's pair in order, e.g. 16 channels for SDI */
} sesame_audio_layout;

typedef enum {
  SESAME_AUDIO_PCM = 0, /* on_audio gets PCM in the descriptor's output audio format */
  SESAME_AUDIO_OPUS = 1 /* on_encoded_audio gets one Opus stream over all bound mixes (ENCODED video only) */
} sesame_audio_delivery;

typedef enum {
  SESAME_PARAM_STRING = 0,
  SESAME_PARAM_INT = 1,
  SESAME_PARAM_FLOAT = 2,
  SESAME_PARAM_BOOL = 3,
  SESAME_PARAM_COLOR = 4, /* "#rrggbb" or "#rrggbbaa" */
  SESAME_PARAM_ENUM = 5,
  SESAME_PARAM_JSON = 6   /* any JSON value, passed compact; for nested or list-valued settings */
} sesame_param_type;

typedef enum {
  SESAME_STATE_OFFLINE = 0,
  SESAME_STATE_INITIALIZING = 1,
  SESAME_STATE_ONLINE = 2,
  SESAME_STATE_ERROR = 3
} sesame_state;

typedef enum { SESAME_LOG_TRACE = 0, SESAME_LOG_INFO = 1, SESAME_LOG_WARNING = 2, SESAME_LOG_ERROR = 3 } sesame_log_level;

/*
 * Numerically identical to sesame.v1.wire.DataType. Plugins publish JSON or
 * BINARY. Outputs bound to engine state also receive the serialized
 * sesame.v1.state frames of the fixed output tracks.
 */
typedef enum {
  SESAME_DATA_JSON = 1,
  SESAME_DATA_BINARY = 6,
  SESAME_DATA_STATE = 12,    /* sesame.v1.state.StateFrame */
  SESAME_DATA_METERS = 13,   /* sesame.v1.state.MetersFrame */
  SESAME_DATA_DESCRIBE = 14, /* sesame.v1.state.DescribeFrame */
  SESAME_DATA_EVENT = 15     /* sesame.v1.state.EventFrame */
} sesame_data_type;

typedef enum {
  SESAME_PUBLISH_STATE = 0, /* kept per (node, metadata ID); newest wins and it is repeated in keyframes */
  SESAME_PUBLISH_EVENT = 1  /* delivered once on the events track, never repeated */
} sesame_publish_mode;

#define SESAME_DATA_MAX_PAYLOAD (1u << 20)
#define SESAME_STATUS_TEXT_MAX 256u
#define SESAME_STATUS_JSON_MAX (64u << 10)

/* Numerically identical to sesame.v1.common.CodecType. */
typedef enum {
  SESAME_CODEC_NONE = 0,
  SESAME_CODEC_VP8 = 1,
  SESAME_CODEC_VP9 = 2,
  SESAME_CODEC_AVC = 3,  /* Annex B */
  SESAME_CODEC_HEVC = 4, /* Annex B */
  SESAME_CODEC_AV1 = 5,  /* OBU sequence, no IVF framing */
  SESAME_CODEC_OPUS = 64,
  SESAME_CODEC_AAC = 65,
  SESAME_CODEC_PCM_S16LE = 66
} sesame_codec;

typedef struct {
  const char* value; /* stored in config */
  const char* label; /* shown in the editor */
} sesame_enum_value;

typedef struct {
  const char* key;
  const char* label;
  sesame_param_type type;
  const char* default_value; /* string form; JSON text for JSON params */
  double min;                /* INT and FLOAT */
  double max;                /* INT and FLOAT */
  const sesame_enum_value* enum_values; /* ENUM */
  uint32_t enum_count;
} sesame_param_def;

typedef struct {
  const char* key;
  const char* value; /* string form, validated by the host against the param def */
} sesame_param;

typedef struct {
  const char* id; /* metadata ID, e.g. "com.example.state.v1" */
  const char* label;
  sesame_bool receive; /* 0 = the plugin publishes it, 1 = the plugin receives it */
  const char* format;  /* "json" or "binary", optionally "json/<name>" */
} sesame_metadata_def;

typedef struct {
  uint32_t struct_size;
  const char* id; /* reverse-domain, globally unique */
  const char* name;
  const char* version;
  sesame_plugin_kind kind;
  sesame_bool has_video;                /* sources; outputs take video when video_delivery is not NONE */
  uint32_t max_audio_channels;          /* 0 = no audio */
  sesame_video_delivery video_delivery; /* outputs only */
  const sesame_param_def* params;
  uint32_t param_count;
  const sesame_metadata_def* metadata;
  uint32_t metadata_count;
  sesame_source_mode source_mode;       /* sources only */
  sesame_audio_delivery audio_delivery; /* outputs only */
  sesame_bool provides_clock;           /* get_clock is implemented */
  uint32_t ring_depth;                  /* sources only: frame slots, 0 = default (4), max 16 */
  sesame_pixel_format output_format;    /* DEVICE and HOST outputs: any pixel format */
  sesame_audio_layout audio_layout;          /* PCM outputs */
  sesame_sample_format output_sample_format; /* PCM outputs */
  sesame_bool output_planar;                 /* PCM outputs */
  uint32_t output_sample_rate;               /* PCM outputs, 0 = the engine rate (48 kHz) */
  /* DEVICE and HOST outputs: PROGRESSIVE, or interlaced frames that each carry two engine frames as fields.
   * Interlaced output needs a format without vertical chroma subsampling (not NV12, P010 or I420). */
  sesame_field_order output_field_order;
  sesame_bool output_field_sequential; /* interlaced outputs: each field's rows stored together, first field first */
  sesame_source_timing source_timing;        /* PUSH sources */
} sesame_plugin_descriptor;

/* Fixed facts about the engine, passed once at create. */
typedef struct {
  uint32_t struct_size;
  uint32_t abi_minor;      /* host ABI minor */
  uint32_t sample_rate;    /* audio sample rate */
  uint32_t audio_channels; /* channels this instance was configured with */
  uint32_t fps_num;        /* frame rate as fps_num/fps_den, e.g. 50/1 or 60000/1001 */
  uint32_t fps_den;
  uint32_t render_width;
  uint32_t render_height;
} sesame_engine_info;

/* Frame layout a source delivers. The host scales it to the source's texture. */
typedef struct {
  uint32_t struct_size;
  uint32_t width;
  uint32_t height;
  sesame_pixel_format pixel_format;
  sesame_color_matrix matrix; /* YUV formats */
  sesame_color_range range;   /* YUV formats and R210 */
  uint32_t pitch_bytes;       /* first-plane row pitch, 0 = the minimum; otherwise at least that and a multiple of 4 */
  sesame_field_order field_order;
  sesame_transfer transfer;
  sesame_primaries primaries;
  /* Interlaced only: the rows of the first field are stored before the rows of the second, as field-based
     transports deliver them, instead of interleaved. Not accepted for 4:2:0 layouts. */
  sesame_bool field_sequential;
} sesame_video_format;

/*
 * One frame slot in a source's ring. The buffers hold one frame in the slot's
 * format; size_bytes covers every plane.
 */
typedef struct sesame_frame_slot {
  uint32_t struct_size;
  uint32_t frame; /* engine frame this slot is for: set by the host in PULL, by the plugin in PUSH */
  uint32_t width;
  uint32_t height;
  sesame_pixel_format pixel_format;
  sesame_color_matrix matrix;
  sesame_color_range range;
  uint32_t pitch_bytes;
  size_t size_bytes;
  uint8_t* host_data;            /* pinned host memory */
  void* device_data;             /* device memory */
  sesame_cuda_stream stream;     /* the source's upload stream; enqueue device writes here */
  uint32_t audio_samples_needed; /* PULL: samples per channel for this frame, at the source's audio rate */
  int64_t budget_us;             /* PULL: time until the frame is needed */
  /* set by the plugin before submit/return */
  sesame_alpha_mode alpha_mode;
  int64_t timecode_frames; /* optional timecode carried with the frame */
  sesame_bool timecode_valid;
  sesame_field_order field_order; /* set by the host from the format */
  sesame_bool field_sequential;
  int64_t device_time_us; /* DEVICE timing: set by the plugin, the frame's time on its own clock */
} sesame_frame_slot;

typedef struct {
  uint32_t struct_size;
  uint64_t now_us;   /* clock reading */
  uint64_t start_us; /* clock value at which frame 0 started; 0 if the clock is absolute */
} sesame_clock_time;

/*
 * One composited frame, in the descriptor's output_format at render size and
 * minimum pitch, planes laid out as for a source frame in that format. YUV and
 * R210 output is BT.709 limited range; UYVA and PA16 carry the composite's alpha
 * as the key. Formats with chroma subsampling need an even render size.
 *
 * An interlaced output gets one frame per two engine frames: the first field is
 * the engine frame `frame`, always even, and the second field the one after it.
 * Audio still arrives once per engine frame.
 */
typedef struct {
  uint32_t struct_size;
  uint32_t frame;
  uint32_t width;
  uint32_t height;
  sesame_pixel_format pixel_format;
  uint32_t pitch_bytes;
  const void* data; /* DEVICE: device memory, HOST: pinned host memory; valid until on_video returns */
  sesame_cuda_stream stream; /* DEVICE: enqueue reads here; the host waits for it before reusing the buffer */
  int64_t timecode_frames; /* interlaced: the first field's */
  sesame_bool timecode_valid;
  sesame_field_order field_order; /* the descriptor's output_field_order */
  sesame_bool field_sequential;
  int64_t clock_time_us; /* engine clock time at which the frame starts; interlaced: the first field's */
} sesame_output_frame;

/* One block of output audio in the descriptor's output audio format; valid until on_audio returns. */
typedef struct {
  uint32_t struct_size;
  const char* mix_id; /* PER_MIX: the mix; MULTICHANNEL: NULL */
  const void* data;   /* interleaved, or planes one after the other */
  uint32_t samples;   /* per channel */
  uint32_t channels;
  sesame_sample_format format;
  sesame_bool planar;
  uint32_t sample_rate;
  int64_t clock_time_us; /* engine clock time of the first sample */
} sesame_output_audio;

/*
 * Data delivered to a plugin. An extension payload carries the metadata ID it
 * was sent under and the node that sent it; an output track frame carries the
 * track name ("state", "meters", "describe" or "events") instead.
 */
typedef struct {
  uint32_t struct_size;
  const char* metadata_id; /* extension payloads; NULL for track frames */
  const char* sender;      /* extension payloads: the source node the control frame arrived on */
  const char* track;       /* track frames; NULL for extension payloads */
  sesame_data_type type;
  const void* payload;
  size_t len;
} sesame_data;

/* One compressed video or audio packet, in either direction. */
typedef struct {
  uint32_t struct_size;
  sesame_codec codec;
  const uint8_t* data;
  size_t size;
  int64_t pts; /* in timebase_num/timebase_den */
  int32_t timebase_num;
  int32_t timebase_den;
  sesame_bool keyframe; /* video */
  uint32_t width;       /* video */
  uint32_t height;
  uint32_t channels; /* audio */
  uint32_t sample_rate;
} sesame_encoded_packet;

typedef struct {
  uint32_t struct_size;
  void (*log)(void* ctx, sesame_log_level level, const char* msg);
  /*
   * Any thread: the instance's state, a line of text (at most
   * SESAME_STATUS_TEXT_MAX bytes) and optionally a JSON object with
   * type-specific detail (at most SESAME_STATUS_JSON_MAX bytes, NULL to clear).
   * Shown in status until the next call. Before the first call a started
   * instance reports ONLINE.
   */
  sesame_bool (*set_status)(void* ctx, sesame_state state, const char* text, const char* json);
  /* Any thread: publish on a metadata ID declared in the descriptor, under this instance's node ID. */
  sesame_bool (*publish_metadata)(void* ctx, const char* metadata_id, sesame_data_type type, const void* payload,
                                  size_t len, sesame_publish_mode mode);

  /*
   * Sources, from create or any plugin thread: the layout of frames in slots
   * acquired after this call. Slots already acquired keep their layout. The
   * default is RGBA8 at render size. 0 for an unsupported format or size.
   */
  sesame_bool (*set_video_format)(void* ctx, const sesame_video_format* format);

  /*
   * Sources, from create or any plugin thread: the format of audio written or
   * pushed after this call. The channel count must match the source's
   * configured audio channels. The default is S16 interleaved at 48 kHz.
   */
  sesame_bool (*set_audio_format)(void* ctx, const sesame_audio_format* format);

  /* PULL sources, inside produce() only: write exactly audio_samples_needed samples per channel, once. */
  sesame_bool (*write_audio)(void* ctx, const void* data, uint32_t samples);

  /*
   * PUSH sources, any plugin thread after start. Submit slots in acquisition
   * order. With DEVICE timing, submit_frame may accept a slot and still drop it
   * (the clocks drifted, or it arrived after its slot); it only returns 0 for
   * a bad slot. push_audio then takes device time.
   */
  sesame_bool (*acquire_frame)(void* ctx, sesame_frame_slot** slot); /* 0 when the ring is full */
  sesame_bool (*submit_frame)(void* ctx, sesame_frame_slot* slot, uint32_t flags); /* SESAME_FRAME_FROM_* */
  sesame_bool (*release_frame)(void* ctx, sesame_frame_slot* slot);                /* give back unused */
  /* Samples per channel in the source's audio format; timestamp_us is the engine time of the first sample. */
  sesame_bool (*push_audio)(void* ctx, const void* data, uint32_t samples, int64_t timestamp_us);

  /* Any source, any thread: the engine's next frame number and how far into it the clock is. */
  sesame_bool (*get_frame_info)(void* ctx, uint32_t* next_frame, int64_t* offset_us);
  /* Audio timestamp (us) at which the given engine frame's audio starts. */
  int64_t (*audio_timestamp_for_frame)(void* ctx, uint32_t frame);

  /*
   * Any instance, any thread: the engine clock time (us) at which an engine
   * frame starts. The engine clock is the clock the engine's frame timer runs
   * from: a source's get_clock when that source is used as clock, otherwise a
   * monotonic host clock. Frames lie on the clock's frame grid, so with an
   * absolute clock such as PTP (start_us = 0) a frame starts at a whole number
   * of frame periods since the clock's epoch and these times are PTP times.
   */
  int64_t (*clock_time_for_frame)(void* ctx, uint32_t frame);

  /*
   * DEVICE-timed sources, from create: the buffer the source needs by default,
   * in engine frames, e.g. to cover a network's jitter. The operator's config
   * overrides it; the host never goes below 2 frames and rounds interlaced
   * sources up to an even count.
   */
  sesame_bool (*set_target_buffer)(void* ctx, uint32_t frames);
} sesame_host;

/* Functions shared by source and output vtables. */
#define SESAME_INSTANCE_COMMON                                                                                   \
  sesame_instance (*create)(const sesame_host* host, void* ctx, const char* id, const sesame_engine_info* info, \
                            const sesame_param* params, uint32_t count);                                        \
  void (*destroy)(sesame_instance self);                                                                        \
  sesame_bool (*start)(sesame_instance self); /* non-blocking; spawn own threads here */                        \
  void (*stop)(sesame_instance self);         /* join own threads; idempotent */                                \
  sesame_bool (*can_update)(sesame_instance self, const sesame_param* params, uint32_t count);                  \
  sesame_bool (*update)(sesame_instance self, const sesame_param* params, uint32_t count);                      \
  void (*on_data)(sesame_instance self, const sesame_data* data); /* optional */

typedef struct {
  uint32_t struct_size;
  SESAME_INSTANCE_COMMON
  /* PULL mode: fill the slot for slot->frame; return SESAME_FRAME_FROM_HOST/DEVICE or SESAME_FRAME_NONE. */
  uint32_t (*produce)(sesame_instance self, sesame_frame_slot* slot);
  /* Sources with provides_clock: read the clock. Called from the engine timer; must be quick and never block. */
  sesame_bool (*get_clock)(sesame_instance self, sesame_clock_time* time);
} sesame_source_vtable;

typedef struct {
  uint32_t struct_size;
  SESAME_INSTANCE_COMMON
  void (*on_video)(sesame_instance self, const sesame_output_frame* f);                /* DEVICE and HOST */
  void (*on_audio)(sesame_instance self, const sesame_output_audio* a);                /* PCM audio */
  void (*on_encoded_video)(sesame_instance self, const sesame_encoded_packet* packet); /* ENCODED video */
  void (*on_encoded_audio)(sesame_instance self, const sesame_encoded_packet* packet); /* OPUS audio */
  /* Outputs with provides_clock: read the clock. Called from the engine timer; must be quick and never block. */
  sesame_bool (*get_clock)(sesame_instance self, sesame_clock_time* time);
} sesame_output_vtable;

typedef struct {
  uint32_t abi_major;
  uint32_t abi_minor;
  sesame_bool (*init)(const char* library_path);
  void (*deinit)(void);
  uint32_t (*descriptor_count)(void);
  const sesame_plugin_descriptor* (*descriptor)(uint32_t index);
  const void* (*vtable)(const char* id); /* sesame_source_vtable* or sesame_output_vtable* */
} sesame_plugin_entry;

#define SESAME_PLUGIN_ENTRY_SYMBOL "sesame_plugin_entry_v1"

#ifdef SESAME_PLUGIN_IMPLEMENTATION
SESAME_EXPORT extern const sesame_plugin_entry sesame_plugin_entry_v1;
#endif

#ifdef __cplusplus
}
#endif

#endif /* SESAME_PLUGIN_H */
