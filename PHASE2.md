# Phase 2: Color Quantization API and Palette Mapping

## Status: DONE

Phase 2a (quantizer API) committed as `8e60ec654f` + `8d7abb5328`.
Phase 2b (palette mapping extraction) committed as `3326aa9602` + `557d01153a`.

No amendments anticipated — the quantization and palette mapping APIs are
consumed by Phase 3 (text-to-bitmap) and future phases (DVD, GIF) as-is.
Animation support in the PGS encoder (Phase 1 amendment) does not require
changes to the quantization layer.

## Phase 2a: Quantizer API + NeuQuant

### Problem

FFmpeg had no shared color quantization API. Each consumer implemented
its own: `dvdsubenc.c` (ad-hoc nearest-color), `vf_palettegen.c`
(Median Cut), `vf_elbg.c` (ELBG). No common interface, no code reuse,
no perceptual color distance.

PGS encoding requires reducing arbitrary RGBA images to ≤256 colors.
DVD requires ≤4 colors. A generic API with configurable palette size
(2-256) serves all bitmap subtitle encoders.

### What Was Built

#### Patch 1/2: OkLab palette utilities move (`8e60ec654f`)

Pure refactor. Moved `palette.{h,c}` from `libavfilter/` to `libavutil/`.
Updated all includes in `vf_palettegen.c` and `vf_paletteuse.c`. No
functional change — FATE tests bit-for-bit identical.

**Why OkLab in libavutil?** The quantizer (libavutil) needs perceptual
color distance. OkLab was already implemented in `vf_paletteuse.c`.
Moving it to libavutil makes it available to the quantizer without
creating a libavutil → libavfilter dependency (which would violate
FFmpeg's library dependency order).

**Files:**
- `libavutil/palette.h` (57 lines) — OkLab ↔ sRGB conversion API
- `libavutil/palette.c` (214 lines) — Fixed-point OkLab with LUT gamma

**Key functions:**
- `ff_srgb_u8_to_oklab_int()` — sRGB uint32 → OkLab fixed-point
- `ff_oklab_int_to_srgb_u8()` — OkLab fixed-point → sRGB uint32
- `ff_lowbias32()` — Hash function for palette cache

**Implementation notes:**
- All arithmetic is fixed-point (int32_t), no floating point
- sRGB ↔ linear via 256-entry LUT (EOTF) and 512-entry LUT (OETF)
- Cube root via quadratic approximation + 2 Halley iterations
- OkLab L range [0, 65535], a/b unbounded signed

#### Patch 2/2: Quantization API + NeuQuant (`8d7abb5328`)

New public API in libavutil for color quantization.

**Public API (`libavutil/quantize.h`, 111 lines):**

```c
enum AVQuantizeAlgorithm {
    AV_QUANTIZE_NEUQUANT,   /* Neural-net quantizer */
};

AVQuantizeContext *av_quantize_alloc(enum AVQuantizeAlgorithm algorithm,
                                     int max_colors);
void av_quantize_freep(AVQuantizeContext **pctx);

int av_quantize_generate_palette(AVQuantizeContext *ctx,
                                  const uint8_t *rgba, int nb_pixels,
                                  uint32_t *palette, int quality);

int av_quantize_apply(AVQuantizeContext *ctx,
                       const uint8_t *rgba, uint8_t *indices,
                       int nb_pixels);
```

**Context (`libavutil/quantize.c`, 118 lines):**

Opaque context wrapping algorithm-specific state. Currently only
NeuQuant; `enum AVQuantizeAlgorithm` is extensible for Median Cut
(Phase 5) and ELBG (Phase 5).

```c
struct AVQuantizeContext {
    enum AVQuantizeAlgorithm algorithm;
    int max_colors;
    NeuQuantContext *nq;
};
```

Input validation at API boundary: max_colors [2, 256], quality [1, 30],
non-null pointers, positive pixel count.

**NeuQuant implementation (`libavutil/neuquant.{h,c}`, 40 + 431 lines):**

Ported from pngnq/neuquant32 (Anthony Dekker 1994, Stuart Coyle 2004,
Kornel Lesiński 2009) with these enhancements:

| Enhancement | Rationale |
|-------------|-----------|
| OkLab color space | Perceptual distance via `ff_srgb_u8_to_oklab_int()` |
| Alpha-aware distance | `colorimportance()` weights by alpha — transparent pixels matter less |
| Heap-allocated state | Thread-safe, no globals (original used static arrays) |
| Variable palette size | `netsize` parameter 2-256 (original fixed at 256) |
| RGBA byte order | FFmpeg convention (not ABGR) |

**Algorithm:**
1. Initialize N neurons evenly spaced on OkLab L axis
2. For each training sample (coprime-stepped pixel sampling):
   - Convert RGBA → OkLab
   - Find best-matching neuron (biased by frequency + alpha importance)
   - Move winner toward sample (learning rate α, decaying)
   - Move neighbors toward sample (radius decaying)
3. After training: unbias network, build L-indexed lookup
4. Mapping: L-bin lookup → bidirectional search → Manhattan distance in OkLab+α

**Copyright chain (preserved in source):**
```
Copyright (c) 1994 Anthony Dekker (original NeuQuant)
Modified for RGBA: Copyright (c) 2004-2006 Stuart Coyle
Rewritten: Copyright (c) 2009 Kornel Lesiński
Adapted for FFmpeg: Copyright (c) 2026 David Connolly
```

Original license is permissive (attribution only), compatible with LGPL 2.1.

**FATE test (`libavutil/tests/quantize.c`, 207 lines):**

Three test cases:
1. `test_basic_quantize` — 64×64 gradient image → 16 colors, verify indices in range and palette non-degenerate
2. `test_small_palette` — 4 distinct pixels → 4 colors, verify mapping
3. `test_error_handling` — Invalid max_colors (0, 257), NULL freep safety

**Build integration:**
- `libavutil/Makefile`: HEADERS += quantize.h, OBJS += quantize.o neuquant.o
- `libavutil/version.h`: MINOR 25 → 26
- `doc/APIchanges`: New public functions listed

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Public API | `av_quantize_*` | Called cross-library (libavfilter, fftools) |
| Opaque context | Struct definition in .c only | ABI stability, can add fields without breaking |
| Algorithm enum | Extensible for Median Cut, ELBG | Phase 5 adds algorithms without API change |
| Quality parameter | 1-30 (inverted samplefac) | User-facing "quality" is more intuitive than NeuQuant's "samplefac" |
| Palette format | 0xAARRGGBB uint32_t | Matches FFmpeg AVPALETTE convention |
| No CONFIG gating | Unconditional compilation | No external dependencies, pure C |

## Phase 2b: Palette Mapping Extraction

### Problem

`vf_paletteuse.c` contained ~570 lines of palette mapping and dithering
code tightly coupled to the filter context. Phase 3 (text-to-bitmap) and
Phase 4 (DVD consolidation) need palette mapping as a standalone utility.

### What Was Built

#### Patch 1/2: Extract palette mapping (`3326aa9602`)

New internal API (`ff_` prefix) in libavutil.

**API (`libavutil/palettemap.h`, 135 lines):**

```c
enum FFDitheringMode {
    FF_DITHERING_NONE,
    FF_DITHERING_BAYER,
    FF_DITHERING_HECKBERT,
    FF_DITHERING_FLOYD_STEINBERG,
    FF_DITHERING_SIERRA2,
    FF_DITHERING_SIERRA2_4A,
    FF_DITHERING_SIERRA3,
    FF_DITHERING_BURKES,
    FF_DITHERING_ATKINSON,
    FF_DITHERING_NB
};

FFPaletteMapContext *ff_palette_map_init(const uint32_t *palette,
                                          int trans_thresh);
void ff_palette_map_uninit(FFPaletteMapContext **pctx);

int ff_palette_map_apply(FFPaletteMapContext *ctx,
                          uint8_t *dst, int dst_linesize,
                          uint32_t *src, int src_linesize,
                          int x_start, int y_start, int w, int h,
                          enum FFDitheringMode dither, int bayer_scale);

int ff_palette_map_color(FFPaletteMapContext *ctx, uint32_t color);
```

**Implementation (`libavutil/palettemap.c`):**

Extracted from `vf_paletteuse.c` with minimal changes:

| Component | Original location | Lines |
|-----------|------------------|-------|
| KD-tree build + search | `colormap_insert`, `colormap_nearest_node` | ~80 |
| Hash cache (32K entries) | `color_get` | ~30 |
| OkLab color distance | `diff` | ~20 |
| 9 dithering algorithms | `set_frame` (Bayer, Heckbert, Floyd-Steinberg, Sierra variants, Burkes, Atkinson) | ~130 |
| Dither color application | `dither_color` | ~10 |

**Data structures (visible in header for stack allocation):**

```c
struct color_info {
    uint32_t srgb;
    int32_t lab[3];   /* OkLab */
};

struct color_node {
    struct color_info c;
    uint8_t palette_id;
    int split;           /* KD-tree axis: 0=L, 1=a, 2=b */
    int left_id, right_id;
};

typedef struct FFPaletteMapContext {
    struct cache_node cache[FF_PALETTE_CACHE_SIZE];
    struct color_node map[AVPALETTE_COUNT];
    uint32_t palette[AVPALETTE_COUNT];
    int transparency_index;
    int trans_thresh;
} FFPaletteMapContext;
```

**Naming decision:** Struct names (`color_info`, `color_node`, etc.) kept
verbatim from `vf_paletteuse.c` to minimize extraction diff. Renaming to
`FFColorInfo` etc. noted as future cleanup in commit message. Not changing
them now avoids mixing cosmetic and functional changes.

**Internal-only API:** `ff_` prefix. The palette mapping is consumed only
by code within FFmpeg libraries (vf_paletteuse.c, future dvdsubenc.c,
future gif.c). No need for public visibility — unlike the quantizer which
is called from fftools.

#### Patch 2/2: Refactor vf_paletteuse (`557d01153a`)

Replaced ~570 lines in `vf_paletteuse.c` with calls to `ff_palette_map_*`.
The filter's `PaletteUseContext` now holds a `FFPaletteMapContext *` instead
of inline KD-tree, cache, and dithering state.

**FATE verification:** All 4 paletteuse tests produce bit-for-bit identical output:
- `fate-filter-paletteuse-nodither`
- `fate-filter-paletteuse-bayer`
- `fate-filter-paletteuse-bayer0`
- `fate-filter-paletteuse-sierra2_4a`

### Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Internal API | `ff_` prefix | Only consumed within FFmpeg libraries |
| Struct visibility | Visible in header | Allows stack/inline allocation in filter context |
| Naming | Keep original names | Minimize extraction diff; rename is cosmetic follow-up |
| Dithering in palettemap | All 9 algorithms | Complete extraction; don't split arbitrarily |
| Heap allocation | `ff_palette_map_init` returns pointer | Original was inline struct; functions now need int return for error propagation |

## Architecture

```
libavutil/palette.{h,c}        OkLab ↔ sRGB conversions
        │
        ├─→ libavutil/neuquant.{h,c}     NeuQuant algorithm (OkLab distance)
        │         │
        │         └─→ libavutil/quantize.{h,c}   Public API wrapper
        │
        └─→ libavutil/palettemap.{h,c}   KD-tree mapping + 9 dithering modes
                  │
                  └─→ libavfilter/vf_paletteuse.c   Consumer (refactored)
```

**Data flow for PGS encoding (Phase 3 consumer):**
1. Render text → RGBA bitmap (libavfilter/subtitle_render.c)
2. `av_quantize_generate_palette()` → train NeuQuant → 256-color palette
3. `av_quantize_apply()` → map pixels to palette indices
4. PGS encoder writes palette as PDS, indices as RLE-encoded ODS

**Data flow for animation (Phase 1 amendment consumer, future):**
1. Render N animation frames → N RGBA bitmaps
2. Quantize all frames against a shared palette (or per-frame palettes)
3. Encode first frame as Epoch Start (full ODS + PDS)
4. Encode subsequent frames as Normal (PDS-only palette update)

The quantizer API is frame-agnostic — it doesn't know about animation.
Animation-aware palette optimization (choosing a palette that works across
multiple frames) is an encoder-level concern, not a quantizer concern.

## Files

### Phase 2a

| File | Status | Role |
|------|--------|------|
| `libavutil/palette.h` | Committed | OkLab conversion API (moved from libavfilter/) |
| `libavutil/palette.c` | Committed | OkLab implementation |
| `libavutil/quantize.h` | Committed | Public quantization API |
| `libavutil/quantize.c` | Committed | API wrapper (dispatches to algorithm) |
| `libavutil/neuquant.h` | Committed | NeuQuant internal API |
| `libavutil/neuquant.c` | Committed | NeuQuant implementation |
| `libavutil/tests/quantize.c` | Committed | FATE test |
| `libavutil/Makefile` | Committed | Build integration |
| `libavutil/version.h` | Committed | Version bump (MINOR 25→26) |
| `doc/APIchanges` | Committed | New public API entry |
| `libavfilter/Makefile` | Committed | Remove palette.o |
| `libavfilter/vf_palettegen.c` | Committed | Update includes |
| `libavfilter/vf_paletteuse.c` | Committed | Update includes |

### Phase 2b

| File | Status | Role |
|------|--------|------|
| `libavutil/palettemap.h` | Committed | Palette mapping internal API |
| `libavutil/palettemap.c` | Committed | KD-tree + dithering implementation |
| `libavutil/Makefile` | Committed | Add palettemap.o |
| `libavfilter/vf_paletteuse.c` | Committed | Refactored to use ff_palette_map_* |

## References

- Björn Ottosson, "A perceptual color space for image processing" (2020) — OkLab
- Anthony Dekker, "Kohonen neural networks for optimal colour quantization" (1994) — NeuQuant
- Kornel Lesiński, neuquant32/pngnq — RGBA-aware NeuQuant rewrite
- Chris Wellons, "Hash Function Prospector" (2018) — lowbias32 hash
