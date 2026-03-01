# Phase 1: HDMV PGS Subtitle Encoder

## Status: DONE (including composition state machine)

Encoder with composition state machine committed as `2cc882f669`.
Produces valid PGS streams accepted by hardware players for static
subtitles, and supports animation via automatic composition state
detection (Epoch Start, Normal, Acquisition Point), palette versioning,
and palette-only Display Sets for fade effects.

## What Was Built

### `libavcodec/pgssubenc.c` — 398 lines

A stateless PGS encoder that accepts `SUBTITLE_BITMAP` rectangles and
emits complete Display Sets. Each subtitle event produces one Display Set
containing all five segment types (PCS, WDS, PDS, ODS, END).

### Encoder capabilities (initial)

| Feature | Status | Notes |
|---------|--------|-------|
| Static subtitle display | DONE | Single Display Set per event |
| Subtitle removal (clear) | DONE | PCS with object_count=0 |
| Up to 2 composition objects | DONE | AABB overlap rejection |
| ODS fragmentation (>64KB) | DONE | First/middle/last sequence flags |
| BT.709 / BT.601 color space | DONE | Auto-select from video height |
| Forced subtitle flag | DONE | AV_SUBTITLE_FLAG_FORCED → 0x40 |
| Frame rate codes | DONE | 6 HDMV rates, AVOption override |
| FATE test | DONE | Encode/decode roundtrip with CRC |

### What the initial encoder does NOT do

Every Display Set is an **Epoch Start** (composition_state = 0x80). This
is correct and sufficient for static subtitles — every event stands alone,
fully self-contained. But it means:

- No palette animation (fades require NORMAL Display Sets with palette_update_flag)
- No position animation (scrolling requires NORMAL Display Sets reusing objects)
- No object reuse across Display Sets (every event re-sends the full bitmap)
- No Acquisition Point Display Sets (needed for random-access seek points)
- No decoder model budgeting (stream may violate buffer constraints)

These limitations are acceptable for an initial submission — the encoder
produces streams that play correctly on all hardware. Animation support
is a separate concern requiring a composition state machine.

## Segment Generation (current implementation)

### PCS (Presentation Composition Segment — 0x16)

```
video_width, video_height     ← avctx->width, avctx->height
frame_rate                    ← fps_to_pgs_code() or AVOption
composition_number            ← increments per call (wraps at 0xFFFF)
composition_state             ← always 0x80 (Epoch Start)
palette_update_flag           ← always 0x00
palette_id                    ← always 0x00
number_of_composition_objects ← h->num_rects (0 for clear, 1-2 for show)
```

Per composition object:
```
object_id          ← rect index (0 or 1)
window_id          ← rect index (0 or 1)
object_cropped_flag ← 0x00 (never cropped)
forced_on_flag     ← from AV_SUBTITLE_FLAG_FORCED
object_h_position  ← rect->x
object_v_position  ← rect->y
```

### WDS (Window Definition Segment — 0x17)

One window per composition object, matching rect position and dimensions.
Windows are only emitted for show events (not clear events).

### PDS (Palette Definition Segment — 0x14)

Single palette from `rects[0]->data[1]`. Palette entries converted from
0xAARRGGBB to YCbCrA using `RGB_TO_Y/U/V_BT709` or `RGB_TO_Y/U/V_CCIR`
macros from `libavutil/colorspace.h`.

```
palette_id      ← 0x00
palette_version ← 0x00
```

### ODS (Object Definition Segment — 0x15)

RLE-encoded bitmap from `rect->data[0]` with `rect->linesize[0]` stride.

Small objects (RLE ≤ 65524 bytes): single ODS with sequence_flag = 0xC0
(first and last).

Large objects: fragmented across multiple ODS segments.
- First: sequence_flag = 0x80, payload up to 65524 bytes (0xFFFF - 11)
- Middle: sequence_flag = 0x00, payload up to 65531 bytes (0xFFFF - 4)
- Last: sequence_flag = 0x40, remaining payload

Object Data Length field (3 bytes) = width(2) + height(2) + RLE data size.

### END (End of Display Set — 0x80)

Zero-length payload. Marks Display Set boundary.

## RLE Encoding

The `pgs_encode_rle()` function implements HDMV RLE per the spec
(Section 9 of pgs-specification.md):

| Pattern | Meaning |
|---------|---------|
| `CC` | 1 pixel of color CC (CC ≠ 0) |
| `CC CC` | 2 pixels of color CC (CC ≠ 0) |
| `00 0L` | L pixels of color 0 (L = 1-63) |
| `00 4H LL` | L pixels of color 0 (L = 64-16383, 14-bit) |
| `00 8L CC` | L pixels of color CC (L = 3-63) |
| `00 CH LL CC` | L pixels of color CC (L = 64-16383, 14-bit) |
| `00 00` | End of line |

Optimization: runs of 1-2 non-zero pixels are written as raw bytes
(1-2 bytes) instead of using the 3-byte run-length form. This is
standard practice and matches the spec's encoding efficiency rules.

## FATE Test

`fate-sub-pgs`: Encode a test bitmap subtitle to PGS, decode it back,
verify the output matches a reference CRC. Uses `pgs_sub.sup` from
FATE samples as input (decode → re-encode → decode → compare).

## Amendment Plan: Animation Support

The following changes are required to support palette animation, position
animation, and spec-compliant stream generation.

### Composition State Machine

The encoder must track state across Display Sets within an epoch.
Three composition states per the HDMV spec:

| State | Value | When Used |
|-------|-------|-----------|
| Epoch Start | 0x80 | New subtitle with different object dimensions, or first subtitle |
| Acquisition Point | 0x40 | Random-access entry point — complete copy of current state |
| Normal | 0x00 | Incremental update — palette-only, position-only, or clear |

**Patent basis:** US20090185789A1 (Panasonic) defines the epoch as the
interval where object dimensions and window definitions are constant.
US8638861B2 (Sony) specifies that Acquisition Point Display Sets must
contain complete copies of all referenced objects and palettes.
US7620297B2 (Panasonic) specifies the decoder model that constrains
when each state transition is valid.

**Epoch boundaries:** A new epoch starts when:
- Object dimensions change (different subtitle text → different bitmap size)
- Window dimensions change
- No previous state exists (first subtitle)

Within an epoch, the encoder may emit:
- Normal Display Sets for palette updates (fades)
- Normal Display Sets for position updates (scrolling)
- Normal Display Sets for clear (object_count = 0)
- Acquisition Point Display Sets at periodic intervals for seek support

### Palette Versioning

The PDS `palette_version` field must increment within an epoch whenever
the palette changes. The current encoder always writes version 0.

For palette animation (e.g., `\fad` fade effects):
1. Epoch Start: full Display Set with version 0
2. Normal updates: PCS with `palette_update_flag = 0x80`, PDS with
   incremented version, no WDS or ODS needed
3. Clear: PCS with object_count = 0

**Patent basis:** US20090185789A1 describes palette_update_flag as
enabling Display Sets that contain only PCS + PDS + END, skipping WDS
and ODS entirely. The decoder retains the previously decoded object
and applies the new palette.

**Constraint:** Maximum 8 palettes per epoch (from spec Section 14).
For a fade effect with N steps, palette version wraps at 8.

### Object Reuse

For position animation (e.g., `\move` in ASS), the object bitmap
doesn't change — only its position in the PCS changes. The encoder
can emit Normal Display Sets that reference a previously-decoded
object_id without re-sending the ODS.

**Patent basis:** US7620297B2 describes the Decoded Object Buffer
(4 MB) that retains decoded objects across Display Sets within an
epoch. Objects with the same object_id and version are reused from
the buffer.

**Constraint:** Object dimensions must not change within an epoch.
If the text changes (different bitmap), a new epoch is required.

### Decoder Buffer Model

The HDMV spec defines a reference decoder model with specific buffer
sizes and transfer rates. A compliant encoder must ensure that segment
timing does not violate buffer constraints.

**From pgs-specification.md Section 11 and patents US7620297B2, US8638861B2:**

| Buffer | Size |
|--------|------|
| Coded Data Buffer | 1 MB (1,048,576 bytes) |
| Decoded Object Buffer | 4 MB (4,194,304 bytes) |
| Composition Buffer | 8 PCS + 8 PDS |
| Graphics Plane | video_width × video_height bytes |

| Transfer Rate | Bytes/sec | Bits/sec | Path |
|---------------|-----------|----------|------|
| Rx | 2,000,000 | 16 Mbps | Stream → Coded Data Buffer |
| Rd | 16,000,000 | 128 Mbps | Coded Data → Decoded Object |
| Rc | 32,000,000 | 256 Mbps | Object Buffer → Graphics Plane |

**Timing formulas (FREQ = 90000, 90 kHz clock):**

Epoch Start (full-screen clear):
```
DECODE_DURATION = ⌈FREQ × video_width × video_height / Rc⌉

1920×1080 example: ⌈90000 × 2073600 / 32000000⌉ = 5832 ticks ≈ 64.8 ms
```

Non-Epoch-Start:
```
DECODE_DURATION = max(window_clear_time, object_decode_time) + window_write_time

window_clear_time  = Σ ⌈FREQ × w_h × w_w / Rc⌉   (per unassigned window)
object_decode_time = Σ ⌈obj_w × obj_h × FREQ / Rd⌉ (per new object)
window_write_time  = Σ ⌈w_h × w_w × FREQ / Rc⌉    (per assigned window)
```

Segment timestamps:
```
DTS(PCS) = PTS(PCS) - DECODE_DURATION
DTS(first_ODS) = DTS(PCS)
PTS(first_ODS) = DTS(PCS) + ⌈obj_w × obj_h × FREQ / Rd⌉
DTS(next_ODS) = PTS(previous_ODS)
PTS/DTS(PDS) = DTS(PCS)         (palette loads are instantaneous)
PTS/DTS(END) = PTS(last_ODS)
```

Coded Data Buffer fills at Rx between segments and drains by segment
size on consumption (leaky bucket model). Underflow = non-compliant.

The encoder must compute DTS values for ODS segments such that all
data is decoded before the PCS presentation time. The current encoder
sets DTS = 0 for all segments, which works but is not spec-compliant.

### Acquisition Point Strategy

For seek support, Acquisition Point Display Sets should be emitted
periodically within long epochs. A reasonable default is every 2-5
seconds (matching typical GOP intervals in video).

Acquisition Points contain complete copies of all currently-displayed
objects and palettes, allowing a decoder to start mid-stream.

### Context Structure Changes

```c
typedef struct PGSSubEncContext {
    AVClass *class;
    int composition_number;
    int frame_rate;

    /* Epoch state (new) */
    int in_epoch;              /* currently within an epoch */
    int epoch_palette_version; /* PDS version counter */
    int epoch_obj_version[2];  /* ODS version per object_id */
    int epoch_obj_w[2];        /* object dimensions for epoch */
    int epoch_obj_h[2];
    uint8_t *epoch_obj_rle[2]; /* cached RLE for object reuse */
    int epoch_obj_rle_size[2];
    uint32_t epoch_palette[256]; /* cached palette for delta */
    int epoch_palette_count;
} PGSSubEncContext;
```

### Encoding Flow (amended)

```
Input: AVSubtitle with bitmap rects

1. Determine composition state:
   - First subtitle or dimension change → EPOCH_START
   - Same dimensions, palette change only → NORMAL + palette_update_flag
   - Same dimensions, position change only → NORMAL (no ODS)
   - Periodic refresh → ACQUISITION_POINT

2. Write PCS with correct composition_state

3. If EPOCH_START or ACQUISITION_POINT:
   - Write WDS, PDS, ODS, END (full Display Set)
   - Cache object RLE and palette

4. If NORMAL with palette_update_flag:
   - Write PCS (palette_update_flag=0x80) + PDS + END
   - No WDS or ODS

5. If NORMAL with position update:
   - Write PCS + WDS + END
   - No PDS or ODS (reuse cached)

6. Compute DTS from decoder model timing formulas

7. Increment composition_number and palette_version as needed
```

## Files

| File | Status | Role |
|------|--------|------|
| `libavcodec/pgssubenc.c` | Committed | Encoder implementation |
| `libavcodec/Makefile` | Committed | Build integration |
| `libavcodec/allcodecs.c` | Committed | Codec registration |
| `tests/fate/subtitles.mak` | Committed | FATE test definition |
| `tests/ref/fate/sub-pgs` | Committed | FATE reference output |

## References

- `docs/pgs-specification.md` — Compiled from patents and reverse engineering
- US20090185789A1 (Panasonic) — Stream shaping, decoder model, composition states
- US8638861B2 (Sony) — Segment syntax, buffering model
- US7620297B2 (Panasonic) — Decoder model, object buffer management
- FFmpeg `libavcodec/pgssubdec.c` — Reference decoder (composition state handling)
- SUPer encoder — Hardware-validated reference for composition state transitions
  and decoder model compliance (cited for spec interpretation, not code patterns)
