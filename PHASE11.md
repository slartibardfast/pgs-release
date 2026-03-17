# Phase 11: Website Updates for v4/v5

Comprehensive update of all five documentation pages to reflect v4 and v5 work,
normalise to British English (prose only, never code identifiers), and fix
cross-page consistency.

## Context

The website currently describes v3 (14 patches) in its Patches section while
acknowledging v5 elsewhere. FATE test count is wrong. Branch links are stale.
Spelling oscillates between British and American English. Footer/links structure
varies between pages.

## Data

- **v5 patch count:** 23 patches on upstream master
- **v5 FATE tests:** 13 (sub-pgs, quantize, api-pgs-fade, api-pgs-animation-util,
  api-pgs-animation-timing, api-pgs-coalesce, api-pgs-rectsplit, gifenc-rgba,
  sub-ocr-roundtrip, api-pgs-palette-delta, api-pgs-dts, api-pgs-overlap-verify,
  sub-pgs-overlap)
- **v5 new features over v4:** palette delta encoding, per-segment DTS in SUP muxer,
  per-packet DTS in fftools, event lookahead window, AV_CODEC_PROP_EXPLICIT_END,
  clear Display Sets
- **v4 over v3:** avpriv_ cross-library prefixes, palettemap_internal.h,
  sub_util moved to libavutil, FF-prefixed structs, doc/encoders.texi,
  independent compilation verified, quantize_method range fix

## Rules

1. **British English in prose, American in code.** Prose: colour, organisation,
   recognised, optimisation. Code identifiers, CSS properties, API names: color,
   quantize, etc. Never change `color-distance/` directory name or CSS `color:`.
2. **Exception: "quantizer" and "quantization"** always American — follows the
   FFmpeg API name `av_quantize_*`. Added to CLAUDE.md Website Style rule.
3. **One commit per page.** Easier to review and revert.
4. **No new content beyond what exists.** Update stale references, fix spelling,
   align structure. Do not add new sections or features.

## Steps

### 11a: development.html — DONE

- [x] Header: "fifth" → "sixth" iteration, Latest v3 → v5 (23 patches)
- [x] Patches section: A(11), B(5), C(1), D(2), E(2), F(2) = 23
  - A: added palette delta bullet
  - B: added sub_util, lookahead, per-packet DTS
  - D: 1 → 2 patches
  - E: renamed "Pipeline Wiring + Muxer", added SUP muxer DTS, clear DS
  - F: new series — Tests + Documentation
- [x] FATE count: 9 → 13, added 4 new test names
- [x] Series v5 evolution: 24 → 23 patches, 11 → 13 FATE, "optimisations"
- [x] British English: colours, Colour distance link
- [x] CLAUDE.md: added Website Style rule with quantizer exception

### 11b: index.html — DONE

- [x] "Color distance" → "Colour distance" link text
- [x] Version tag n8.0.1-pgs3.0 confirmed current

### 11c: quantizers/index.html — DONE

- [x] Patches link: pgs3 → pgs5
- [x] Title: "Colour Quantization" (colour for prose, quantization exception)
- [x] All table headings/cells: "color" → "colour" (16/64/256-colour palette)
- [x] Prose: unique colours, many colours
- [x] "Colour distance metrics" link text

### 11d: color-distance/index.html — DONE

- [x] Dead link removed: phase4-dvd-investigation branch
- [x] Title: "Colour Distance Metrics"
- [x] Prose: colours, nearest-neighbour, grey
- [x] JS table header: "Colour"
- [x] Paper citation kept in original language

### 11e: ocr-languages/index.html — DONE

- [x] Added Links section with CSS (.links) before footer
- [x] Links: main site, development, tessdata
- [x] No prose "color" instances to fix (all CSS)

### 11f: Cross-page review — DONE

- [x] All inter-page links verified (all exist)
- [x] All pages have back-links to parent
- [x] All pages have Links section before footer
- [x] All pages have footer
- [x] No dead links
