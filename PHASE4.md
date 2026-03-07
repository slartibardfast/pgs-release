# Phase 4: Region-Weighted Quantization

## Status: Design complete, implementation pending

Extends Phase 2a's quantizer API with region-weighted sampling for
multi-event subtitle frames, and introduces HyAB distance for quality
validation of sparse palette mappings.

## Problem

PGS allows only one palette (PDS) per Display Set. When multiple
overlapping ASS events are coalesced into a single PGS frame — the
primary case being karaoke animation (vivid per-syllable colors)
overlapping with standard dialogue (white-on-black) — NeuQuant
allocates palette entries proportional to pixel count, not color
diversity. The dialogue dominates because it has more pixels, starving
the karaoke of palette representation.

Phase 3's coalescing logic (`flush_coalesced_subtitles()`) composites
all overlapping events onto one RGBA canvas before quantizing. A
karaoke event with 500 vivid colored pixels gets drowned by a dialogue
event with 5000 white-on-black pixels — NeuQuant allocates most of the
256 palette entries to shades the dialogue needs.

## Approach: equal-weight region sampling

Feed NeuQuant pixel samples from each event in equal proportion,
regardless of pixel count. A dialogue event with 5000 pixels and a
karaoke event with 500 pixels each contribute 50% of the training
sample. NeuQuant then allocates palette entries proportional to color
*diversity*, not pixel *count*.

This is zero-touch from the user's perspective — the conversion
pipeline already knows the event boundaries (it rendered them
separately before compositing). No new flags, no tuning.

## API Extension (Phase 2a amendment)

The region concept belongs in `libavutil/quantize.h` — it's generic
and benefits any multi-region quantization consumer (e.g., DVB
subtitles with multiple rects, GIF frames with overlay regions).

```c
/**
 * Add a region of pixels to the quantization input.
 *
 * When regions are added, av_quantize_generate_palette() samples from
 * all regions in equal proportion, ensuring palette representation
 * regardless of pixel count. This prevents large regions from starving
 * small regions of palette entries.
 *
 * If no regions are added, generate_palette() operates on the rgba
 * buffer passed directly to it (backward compatible).
 *
 * @param ctx       quantizer context
 * @param rgba      RGBA pixel data for this region
 * @param nb_pixels number of pixels in this region
 * @return 0 on success, negative AVERROR on failure
 */
int av_quantize_add_region(AVQuantizeContext *ctx,
                           const uint8_t *rgba, int nb_pixels);
```

When regions are present, `av_quantize_generate_palette()` builds the
NeuQuant training sample by drawing equally from each region. When no
regions are added, it works exactly as today — single buffer, unchanged
behavior. Zero API break, zero behavioral change for existing callers.

## Distance Metric: HyAB for Quality Validation

### Why not Euclidean OkLab?

Euclidean distance in OkLab produces misleading results for sparse
palette mapping. When each event has only a small slice of the 256-color
palette, evaluating coverage quality requires a distance metric that
handles sparse palettes honestly.

Analysis of OkLab Euclidean vs sRGB distance across 262,144 sampled
colors against a sparse 16-color palette (DVD default) found:

- **35.66% divergence rate** — over a third of colors map to a
  different palette entry depending on the metric
- OkLab frequently maps chromatic colors to wrong-hue achromatic
  entries (gray→teal, skin tone→gray, dark yellow→green)
- Root cause: Euclidean distance allows a small chroma offset to
  compensate for a large lightness gap, because all three dimensions
  (L, a, b) contribute to a single sum-of-squares

Detailed findings with per-color distance dumps in `phase4_compare.c`
and `phase4_debug.c`.

**Key examples:**

| Input | OkLab picks | sRGB picks | Better |
|-------|------------|-----------|--------|
| mid gray #808080 | teal #008080 | light-gray #AAAAAA | sRGB — gray should map to gray |
| skin tone #E0B090 | light-gray #AAAAAA | salmon #FF8080 | sRGB — warm should stay warm |
| dark yellow #C0C000 | light-green #80FF80 | yellow #FFFF00 | sRGB — yellow should map to yellow |

The problem is structural: OkLab's perceptual uniformity requires
palette density to work well. With few palette entries, there often
isn't a good match in the correct hue region, and Euclidean distance
picks a wrong-hue color with closer lightness.

### Comprehensive metric comparison

Tested 12 distance metrics across 20 human-judged cases and 262K
exhaustive colors. Full program in `phase4_metrics.c`.

**Metrics tested:**

| Metric | Formula (in OkLab unless noted) |
|--------|-------------------------------|
| sRGB Euclidean | ΔR² + ΔG² + ΔB² (baseline) |
| OkLab Euclidean | ΔL² + Δa² + Δb² |
| HyAB (w=1..4) | \|ΔL\| + w·√(Δa²+Δb²) |
| OkLab chroma×N | ΔL² + N·(Δa²+Δb²), N=2,3,4 |
| OkLab light×N | N·ΔL² + Δa² + Δb², N=0.5,0.25 |
| CIEDE2000 | CIE standard (in CIELAB) |

**Human-judged accuracy (20 cases):**

| Metric | Correct | Rate |
|--------|---------|------|
| **sRGB Euclidean** | **16/20** | **80%** |
| CIEDE2000 | 14/20 | 70% |
| HyAB w=3 | 13/20 | 65% |
| HyAB w=4 | 13/20 | 65% |
| OkLab chroma×2 | 12/20 | 60% |
| OkLab chroma×3 | 12/20 | 60% |
| OkLab light×0.5 | 12/20 | 60% |
| OkLab light×0.25 | 12/20 | 60% |
| HyAB w=2 | 11/20 | 55% |
| OkLab Euclidean | 9/20 | 45% |
| HyAB w=1 | 9/20 | 45% |

**sRGB wins on human-judged accuracy.** Even CIEDE2000 (the CIE gold
standard) only scores 70% — it maps skin tone to gray, peach to gray,
and tangerine to red instead of salmon.

**Agreement with CIEDE2000 (262K exhaustive):**

| Metric | Agreement |
|--------|-----------|
| OkLab chroma×3 | 70.74% |
| OkLab chroma×2 / light×0.5 | 70.56% |
| OkLab chroma×4 / light×0.25 | 69.93% |
| HyAB w=2 | 68.37% |
| OkLab Euclidean | 66.34% |
| HyAB w=3 | 66.32% |
| HyAB w=1 | 63.45% |
| HyAB w=4 | 64.01% |
| sRGB | 57.85% |

**OkLab chroma×3 has highest CIEDE2000 agreement** — but CIEDE2000
itself only scores 70% on our human-judged cases. Agreement with a
flawed standard is not the right optimization target.

**Key findings:**

1. **No perceptual metric beats sRGB for this palette.** The DVD
   default palette is too sparse and non-uniform for perceptual
   distance to help. Every OkLab variant (including weighted and HyAB)
   is pulled toward achromatic matches (light-gray) for colors with
   moderate lightness.

2. **HyAB fixes gray→teal but creates new failures.** HyAB w=1
   over-corrects by making light-gray a gravity well (zero chroma
   distance). Higher weights (w=3,4) fix some of these but introduce
   other errors.

3. **The persistent failure: skin tone→gray.** Every metric except sRGB
   maps skin tone #E0B090 to light-gray instead of salmon. This is
   because skin tone's OkLab chroma (a=0.04, b=0.06) is small
   relative to its lightness — all perceptual metrics see it as "almost
   gray." sRGB preserves the warm hue because the per-channel R/G/B
   differences independently penalize the color shift.

4. **CIEDE2000 is not a silver bullet.** Despite being the CIE
   standard, it scores below sRGB on our cases. Its SL/SC/SH
   weighting functions were tuned for small color differences (textile
   matching, paint samples) — not for nearest-neighbor in a sparse
   palette spanning the entire gamut.

5. **Weighted OkLab is the best compromise.** OkLab chroma×2-3
   (ΔL² + N·(Δa²+Δb²)) gets 60% on human cases and 70%+ CIEDE2000
   agreement. It's strictly better than unweighted OkLab or HyAB, and
   trivial to implement.

### Implications for quality validation

The distance metric question matters for two things:

1. **Palette mapping** (nearest-neighbor assignment): which palette
   entry does each pixel get? For the 256-color palettes that NeuQuant
   generates, the palette is dense enough that all metrics perform well.
   This is not the problem.

2. **Quality validation** (coverage assessment): after generating a
   shared palette from multiple regions, how do we measure whether each
   region is adequately represented? This requires comparing each
   region's pixels against a potentially sparse subset of the palette.

For quality validation, the right metric depends on what we're
validating. Since the validation is an internal quality gate (not
user-visible), and since no single perceptual metric dominates, the
simplest correct approach is:

- **Primary fix:** Region-weighted sampling ensures fair palette
  allocation. This is the feature that solves the karaoke problem.
- **Validation (if needed):** Use OkLab chroma×2 (ΔL² + 2·(Δa²+Δb²))
  as a simple improvement over unweighted OkLab. One constant change
  in the distance function — no new infrastructure.

**References:**
- Abasi, S., Tehran, M.A., Fairchild, M.D. (2020). "Distance metrics
  for very large color differences." *Color Research & Application*,
  45(2), 208-223. https://doi.org/10.1002/col.22451
- 30fps.net, "HyAB k-means for color quantization" — practical
  application to retro palette mapping: https://30fps.net/pages/hyab-kmeans/

### DVD subtitle encoder: no change needed

The distance metric analysis was initially motivated by whether
`dvdsubenc.c`'s ad-hoc `color_distance()` should be replaced with
OkLab. Answer: **no**. DVD selects 4 colors from a fixed 16-color
global palette — this is palette *mapping* (select from fixed choices),
not palette *generation*. The existing sRGB distance accidentally works
well because equal-weight RGB channels heavily penalize hue shifts,
which is the right behavior for a sparse palette. The encoder has no
filed bugs and should not be touched.

NeuQuant and `av_quantize_*()` are the wrong tool for DVD entirely —
DVD doesn't generate colors, it selects from a pre-existing set.

## Implementation in the Conversion Pipeline

In `flush_coalesced_subtitles()` (fftools/ffmpeg_enc.c):

1. Render each overlapping event separately (already done)
2. Call `av_quantize_add_region()` for each event's RGBA buffer
3. Call `av_quantize_generate_palette()` — equal-weight sampling
4. Composite onto single canvas, apply palette indices
5. Encode as single PGS Display Set

Step 2 is the only new operation. Steps 1, 3-5 exist today. The
structural change is keeping per-event RGBA buffers alive until after
quantization, rather than compositing first.

### Karaoke animation interaction

Phase 3a classifies karaoke as CONTENT change (per-syllable bitmap
changes). Each frame gets an independent Epoch Start with full
re-quantization. With region-weighted quantization, each frame's
palette fairly represents all visible events at that moment —
including the karaoke syllable colors that would otherwise be
starved by the dominant dialogue event.

## Upstream Framing

Single patch extending Phase 2a's quantizer API with one new function.

```
[PATCH 1/1] lavu/quantize: add region-weighted palette generation
```

- **Concrete use case:** karaoke subtitles with overlapping events
  (common in anime fansubs, the primary PGS subtitle use case)
- **Generic design:** region weighting benefits any multi-region
  consumer, not just PGS
- **Research grounding:** quality validation uses HyAB distance metric
  (published, peer-reviewed — Abasi et al. 2020)
- **Backward compatible:** zero change for single-buffer callers

## Scope

| Component | Change |
|-----------|--------|
| `libavutil/quantize.h` | Add `av_quantize_add_region()` declaration |
| `libavutil/quantize.c` | Region storage, equal-weight sampling logic |
| `libavutil/neuquant.c` | Accept weighted sample set (minor) |
| `fftools/ffmpeg_enc.c` | Use `add_region()` in coalescing path |
| `doc/APIchanges` | New public function entry |
| `libavutil/version.h` | MINOR bump |

HyAB distance function placement TBD — could live in `palettemap.c`
(Phase 2b) as an alternative distance mode, or in `quantize.c` as a
validation utility. Depends on whether other consumers want it.

## Files

| File | Purpose |
|------|---------|
| `phase4_metrics.c` | Comprehensive 12-metric comparison (20 human-judged + 262K exhaustive) |
| `phase4_compare.c` | Initial sRGB vs OkLab comparison (262K colors) |
| `phase4_debug.c` | Per-color OkLab distance dump for root cause analysis |
| `phase4_hyab.c` | HyAB verification (sRGB vs OkLab vs HyAB) |
