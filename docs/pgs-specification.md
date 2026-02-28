# PGS (Presentation Graphic Stream) Specification

Comprehensive technical reference for the HDMV Presentation Graphic Stream format
used for bitmap subtitles on Blu-ray Discs, stored in `.sup` files.

Compiled from:
- US Patent US20090185789A1 / US8350870B2 (Panasonic — McCrossan, Okada, Ogawa)
- US Patent US8638861B2 (Sony — segment syntax, buffering)
- US Patent US7620297B2 (Panasonic — decoder model, buffer management)
- FFmpeg `libavcodec/pgssubdec.c` (reference decoder, Stephen Backway, LGPL 2.1)
- Scorpius blog (practical format breakdown)
- Doom9 forum reverse-engineering threads

---

## 1. Overview

PGS encodes bitmap-based subtitles as a sequence of **Display Sets**, each
composed of **Segments**. Subtitles are 8-bit indexed-color bitmaps with a
256-entry YCbCrA palette. The format supports up to 2 simultaneous objects
on screen, palette animations (fades), and cropping-based effects (scrolling,
wipes).

The stream is multiplexed into MPEG-2 Transport Stream PES packets for
Blu-ray disc storage. When ripped/extracted, segments are concatenated
into a `.sup` file with the "PG" magic header prepended to each segment.

---

## 2. Segment Header

Every segment begins with a **13-byte header**:

```
Offset  Size  Field
------  ----  -----
0x00    2     Magic Number: 0x50 0x47 ("PG")
0x02    4     PTS (Presentation Time Stamp), 90 kHz clock, big-endian
0x06    4     DTS (Decoding Time Stamp), 90 kHz clock, big-endian
0x0A    1     Segment Type
0x0B    2     Segment Size (payload length following this header)
```

**Total header: 13 bytes. Payload follows immediately.**

### Timestamp Encoding

- PTS and DTS are unsigned 32-bit values at 90 kHz resolution
- To convert PTS to milliseconds: `ms = PTS / 90`
- To convert PTS to seconds: `s = PTS / 90000`
- DTS is typically 0x00000000 (unused) except for ODS segments

---

## 3. Segment Types

| Code | Name | Description |
|------|------|-------------|
| 0x16 | PCS | Presentation Composition Segment — display control |
| 0x17 | WDS | Window Definition Segment — display area rectangles |
| 0x14 | PDS | Palette Definition Segment — color table |
| 0x15 | ODS | Object Definition Segment — RLE bitmap data |
| 0x80 | END | End of Display Set — terminator (zero-length payload) |

---

## 4. Display Sets

A Display Set is the atomic unit of PGS composition. It groups all segments
needed to render one screen of subtitles.

### Segment Order Within a Display Set

```
PCS → WDS → PDS → ODS [→ ODS...] → END
```

- Exactly one PCS per Display Set
- WDS is optional if `palette_update_flag` is set (palette-only update)
- Multiple ODS segments may appear (for multi-object or fragmented objects)
- END segment has zero-length payload

### Display Set Types (Composition State)

| Value | Name | Description |
|-------|------|-------------|
| 0x00 | Normal | Incremental update — contains only differences from previous |
| 0x40 | Acquisition Point | Complete refresh — safe entry point for seeking |
| 0x80 | Epoch Start | New epoch — clears all buffers and starts fresh |

The composition state is encoded in the **top 2 bits** of the composition
state byte in the PCS. FFmpeg extracts it as `byte >> 6`.

### Epochs

An **Epoch** is the interval between two Epoch Start display sets. Within an
epoch:

- All objects sharing the same `object_id` must have identical width and height
- Window positions and dimensions remain constant
- Object dimensions can change only at epoch boundaries
- Graphics synchronization is guaranteed within an epoch but not across epochs

### Subtitle Display / Removal

- A PCS with `object_count > 0` and composition state 0x80 shows a subtitle
- A subsequent PCS with `object_count = 0` and composition state 0x00 removes it
- The removal PCS typically has its PTS set to the end time of the subtitle

---

## 5. Presentation Composition Segment (PCS) — 0x16

```
Offset  Size  Field
------  ----  -----
0x00    2     Video Width (e.g., 0x0780 = 1920)
0x02    2     Video Height (e.g., 0x0438 = 1080)
0x04    1     Frame Rate (always 0x10 — ignored by decoders)
0x05    2     Composition Number (increments per update within epoch)
0x07    1     Composition State (0x00=Normal, 0x40=Acquisition, 0x80=Epoch Start)
0x08    1     Palette Update Flag (0x00=False, 0x80=True)
0x09    1     Palette ID (reference to PDS)
0x0A    1     Number of Composition Objects (0–2)
```

Followed by `N` Composition Object entries (8 bytes each, or 16 if cropped):

```
Offset  Size  Field
------  ----  -----
0x00    2     Object ID (reference to ODS)
0x01    1     Window ID (reference to WDS)
0x03    1     Composition Flag
                bit 7 (0x80): Object Cropped
                bit 6 (0x40): Forced On (display regardless of user subtitle setting)
0x04    2     Object Horizontal Position (x)
0x06    2     Object Vertical Position (y)
```

If Cropped flag (bit 7) is set, 8 additional bytes follow:

```
0x08    2     Crop Horizontal Position
0x0A    2     Crop Vertical Position
0x0C    2     Crop Width
0x0E    2     Crop Height
```

### Notes

- Maximum of **2 composition objects** per PCS (`MAX_OBJECT_REFS` in FFmpeg)
- Forced subtitles (flag 0x40) display even when user has subtitles disabled
- The composition number should increment for each new display set in an epoch

---

## 6. Window Definition Segment (WDS) — 0x17

```
Offset  Size  Field
------  ----  -----
0x00    1     Number of Windows
```

Followed by `N` window definitions (9 bytes each):

```
Offset  Size  Field
------  ----  -----
0x00    1     Window ID
0x01    2     Window Horizontal Position (x)
0x03    2     Window Vertical Position (y)
0x05    2     Window Width
0x07    2     Window Height
```

### Constraints

- Window position and dimensions are fixed within an epoch
- Windows should be at least ~25–33% of the graphics plane (per patent)
- Position range: 0 to `video_width - 1` / `video_height - 1`
- A maximum of 2 windows per display set (matching the 2-object limit)

---

## 7. Palette Definition Segment (PDS) — 0x14

```
Offset  Size  Field
------  ----  -----
0x00    1     Palette ID
0x01    1     Palette Version Number (increments within epoch)
```

Followed by palette entries (5 bytes each). The number of entries is
determined by `(segment_size - 2) / 5`:

```
Offset  Size  Field
------  ----  -----
0x00    1     Palette Entry ID (color index 0–255)
0x01    1     Y  (Luminance, 0–255)
0x02    1     Cr (Red difference, 0–255)
0x03    1     Cb (Blue difference, 0–255)
0x04    1     Alpha (0 = fully transparent, 255 = fully opaque)
```

### Color Space

- Colors are encoded in **YCbCr** color space, not RGB
- **BT.709** coefficients for HD content (video height > 576)
- **BT.601** coefficients for SD content (video height ≤ 576)
- FFmpeg auto-selects colorspace based on `avctx->height`
- Maximum 256 palette entries (8-bit color depth)
- Maximum **8 palettes per epoch** (`MAX_EPOCH_PALETTES` in FFmpeg)

### YCbCr to RGB Conversion (BT.709)

```
R = Y + 1.5748 × (Cr - 128)
G = Y - 0.1873 × (Cb - 128) - 0.4681 × (Cr - 128)
B = Y + 1.8556 × (Cb - 128)
```

### Special Values

- Entry ID 0 with Y=0, Cr=128, Cb=128, Alpha=0 typically represents transparent
- Only entries explicitly listed in the PDS are defined; others are undefined

---

## 8. Object Definition Segment (ODS) — 0x15

```
Offset  Size  Field
------  ----  -----
0x00    2     Object ID
0x02    1     Object Version Number
0x03    1     Sequence Flag
                0x80 = First in sequence
                0x40 = Last in sequence
                0xC0 = First and Last (single segment, complete object)
0x04    3     Object Data Length (includes 4 bytes for width + height)
0x07    2     Object Width    (only in first segment)
0x09    2     Object Height   (only in first segment)
0x0B    ...   RLE Data        (variable length)
```

### Fragmentation

Objects larger than a single segment's capacity are split across multiple
ODS segments:

- First segment: flag = 0x80, includes width/height/RLE data
- Middle segments: flag = 0x00, includes only RLE data continuation
- Last segment: flag = 0x40, includes final RLE data

The `Object Data Length` field (3 bytes, big-endian) in the first segment
indicates the total size of width (2) + height (2) + all RLE data across
all fragments. Subtract 4 to get the RLE data length.

### Constraints

- Maximum **64 objects per epoch** (`MAX_EPOCH_OBJECTS` in FFmpeg)
- Maximum segment payload: 0xFFFF bytes (16-bit segment size field)
- Objects with the same `object_id` within an epoch must have identical dimensions
- Object dimensions must not exceed video dimensions
- Bitmap dimensions must be non-zero

---

## 9. Run-Length Encoding (RLE)

The bitmap data in ODS is compressed using a custom run-length encoding.
Each pixel is an 8-bit palette index. Runs are encoded as follows:

### Encoding Rules

| Byte Pattern | Meaning |
|-------------|---------|
| `CC` | 1 pixel of color `CC` (where CC ≠ 0x00) |
| `00 00` | End of line |
| `00 0L` | `L` pixels of color 0 (L = 1–63, bits 5:0) |
| `00 4L LL` | `L` pixels of color 0 (L = 64–16383, 14-bit: bits 5:0 of first byte + second byte) |
| `00 8L CC` | `L` pixels of color `CC` (L = 3–63, bits 5:0) |
| `00 CL LL CC` | `L` pixels of color `CC` (L = 64–16383, 14-bit: bits 5:0 of first byte + second byte) |

### Decoding Algorithm (from FFmpeg)

```
read color byte
if color != 0x00:
    run = 1, output 1 pixel of color
else:
    read flags byte
    run = flags & 0x3F               (low 6 bits)
    if flags & 0x40:                 (extended length)
        run = (run << 8) | next_byte (14-bit total)
    if flags & 0x80:                 (non-zero color)
        color = next_byte
    else:
        color = 0                    (transparent/color 0)
    if run == 0:
        end of line
    else:
        output run pixels of color
```

### Bit Flag Summary

```
Flags byte: [C][E][LLLLLL]
             |  |  └── Low 6 bits of run length
             |  └── Extended: 1 = read another byte for full 14-bit length
             └── Color: 1 = read color byte; 0 = color is 0
```

### Line Structure

- Each scanline is independently encoded
- Lines are terminated by `00 00` (end-of-line marker)
- The last line must also end with `00 00`
- Pixel count per line should exactly equal object width

### Encoding Optimization (Short Runs)

For non-zero colors with run length 1 or 2, the color byte itself is
written directly (without the `00` prefix). This is more compact for
scattered pixels:

- Run of 1: write `CC` (1 byte instead of 3)
- Run of 2: write `CC CC` (2 bytes instead of 3)
- Run of 3+: write `00 8L CC` (3 bytes)

---

## 10. End of Display Set Segment — 0x80

```
Offset  Size  Field
------  ----  -----
(none — zero-length payload)
```

The END segment marks the completion of a display set. Its segment size is 0.

---

## 11. Decoder Buffer Model

### Buffers

| Buffer | Size | Description |
|--------|------|-------------|
| Graphics Plane | ~2 MB (1920×1080×8 bits) | Final rendered output |
| Object Buffer | 4–8 MB | Stores decoded RLE objects |
| Composition Buffer | 8 PCS + 8 PDS | Active composition state |
| Coded Data Buffer | Variable | Incoming PES packets |
| CLUT | 256 × 4 bytes | Active color lookup table |

### Transfer Rates

| Rate | Value | Description |
|------|-------|-------------|
| Rc | 256,000,000 bytes/sec | Object Buffer → Graphics Plane |
| Rd | 128 Mbps | Coded Data → Object Buffer (decode rate) |

### Timing Constraints

```
PTS(PCS) = DTS(PCS) + DECODE_DURATION
DECODE_DURATION = clear_time + decode_time + write_time

clear_time = 90000 × (video_width × video_height) / 256000000
write_time = 90000 × (window_size) / 256000000   [per window]

PTS(ODS) = DTS(ODS) + ⌈90000 × SIZE(ODS) / Rd⌉
```

### Maximum Stream Bitrate

- Recommended range: 500–16000 kbps for compliant streams
- The largest object in an epoch determines the decoding delay
- Events may need to be dropped to maintain compliant bitrate

---

## 12. Supported Resolutions

| Resolution | Width | Height | Use |
|-----------|-------|--------|-----|
| 1080p/i | 1920 | 1080 | Full HD Blu-ray |
| 720p | 1280 | 720 | HD Blu-ray |
| 480p/i (NTSC) | 720 | 480 | SD Blu-ray |
| 576p/i (PAL) | 720 | 576 | SD Blu-ray |

The graphics plane matches the video resolution. SD content (≤576 lines)
uses BT.601 color space; HD content uses BT.709.

---

## 13. Effects

PGS supports visual effects through composition manipulation across
sequential display sets:

| Effect | Mechanism |
|--------|-----------|
| Cut-in/out | Toggle object visibility via `composition_flag` |
| Fade in/out | Modify palette alpha/luminance across display sets |
| Scroll | Incrementally adjust crop rectangle position |
| Wipe | Progressively change crop dimensions |
| Color change | Palette-only update (set `palette_update_flag`) |

---

## 14. Maximum Limits Summary

| Resource | Limit |
|----------|-------|
| Simultaneous display objects | 2 |
| Objects per epoch | 64 |
| Palettes per epoch | 8 |
| Palette entries | 256 |
| Color depth | 8-bit indexed |
| Alpha levels | 256 (8-bit) |
| Max segment payload | 65535 bytes (0xFFFF) |
| ODS data length | 16777215 bytes (0xFFFFFF, 24-bit field) |
| Max RLE run length | 16383 (14-bit) |
| Graphics memory | 4 MB |
| Subtitle streams per disc | 32 |
| PTS/DTS clock | 90 kHz (unsigned 32-bit) |

---

## 15. .sup File Format

A `.sup` file is simply the concatenation of all PGS segments with their
13-byte headers. There is no file-level header or index — segments are
read sequentially. The file can be identified by the `PG` (0x5047) magic
bytes at the start.

When extracted from M2TS containers, PES packet boundaries are removed
and segments are laid out contiguously. Professional authoring tools
(Scenarist) may use `.pes` format which retains the PES packet structure.

### Typical File Structure

```
[PG header + PCS] [PG header + WDS] [PG header + PDS] [PG header + ODS] [PG header + END]
[PG header + PCS] [PG header + WDS] [PG header + PDS] [PG header + ODS] [PG header + END]
...
```

Each `[PG header + segment]` block is 13 + segment_size bytes.
