# Phase 10: PGS Encoder Optimizations (v5)

## Status: In Progress

Encoder improvements beyond the review-mandated fixes in v4.

## 10a: Palette Delta Encoding — DONE

Cache YCbCrA palette entries after each PDS write. On palette-only
updates (Normal DS with palette_update_flag), write only entries
that changed since the last PDS. Full palette on Epoch Start and
Acquisition Point.

For a 16-color subtitle fade at 24fps over 72 frames:
- Before: 72 × 82 bytes = ~5.9 KB of PDS data
- After:  72 × 7 bytes  = ~0.5 KB of PDS data (90% reduction)

## 10b: DTS Computation — PLANNED

Compute proper DTS for PGS packets per the HDMV timing model.
Currently `DTS = PTS`; spec says `DTS = PTS - decode_duration`.
The encoder already computes `decode_duration` — needs a mechanism
to pass it to the muxer.

## 10c: Event Lookahead Window — PLANNED

Buffer subtitle events with full time spans. Compute change points
where the set of visible subtitles changes. Re-render at each
change point. Handles overlapping events with different durations.
See PHASE9-LOOKAHEAD.md for design.

## Known Issues

### ASS fade encoding regression — FIXED
ASS input with `\fad` tags produces "Invalid argument" from the
PGS encoder after the v5 lookahead window changes. Simple SRT and
non-animated ASS work correctly. The FATE api-pgs-fade test passes
(uses the encoder directly), so the issue is in the fftools→encoder
pipeline path for animated events. Needs investigation.

Fixed: third ff_sub_render_event call in alpha animation path
needed events_loaded guard, matching the two in Pass 1/2.
