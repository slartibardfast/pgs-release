# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

"Work as if you live in the early days of a better world."

## Project Overview

PunkGraphicStream (v0.7.2) converts ASS (Advanced SubStation Alpha) subtitle files into PGS/SUP (Presentation Graphic Stream) subtitle files for Blu-ray discs. It is a Java 1.6 application with a native C library (libpgs-jni) wrapping libass via JNI.

## Repository Structure

Parent project with four git submodules:
- `punkgraphicstream/` - Main Java application (Maven project)
- `libpgs-jni/` - Native C JNI wrapper around libass
- `libass/` - libass library (pinned to 0.17.4)
- `ffmpeg/` - FFmpeg (our upstream contribution branch)

Key files:
- `PLAN.md` - Master implementation plan for FFmpeg upstream contribution (read this first for FFmpeg work)
- `docs/pgs-specification.md` - PGS format specification

## Build Commands

All commands run from `punkgraphicstream/`:

```bash
mvn compile              # Compile
mvn package              # Build JAR (target/pgs-0.0.1-SNAPSHOT.jar)
mvn test                 # Run tests (JUnit 4.11)
mvn test -Dtest=MyTest   # Run single test class
mvn clean                # Clean build artifacts
```

Compiler warnings (`-Xlint:unchecked`, `-Xlint:deprecation`) are enabled.

### FFmpeg (from `ffmpeg/`)

```bash
cd ffmpeg && ./configure --disable-doc && make -j$(nproc)  # Build
FATE_SAMPLES=/tmp/fate-samples make fate-sub-pgs            # Run PGS FATE test
```

FFmpeg style: 4-space indent, no tabs, 80-char lines, K&R braces, `snake_case` functions.

## Architecture

### Processing Pipeline (multi-threaded)

```
ASS file → RenderRunnable → QuantizeRunnable (N threads) → EncodeRunnable → .sup file
              (JNI/libass)     (NeuQuant color reduction)    (PGS encoding)
```

- **RenderRunnable**: Opens ASS via JNI, iterates timecodes, renders frames to BufferedImages, detects animation changes
- **QuantizeRunnable**: Color-quantizes images using NeuQuant neural-net algorithm (CPU count threads)
- **EncodeRunnable**: Encodes quantized frames to PGS/SUP format via SupGenerator

Threads coordinate via blocking queues, semaphores, and AtomicBoolean for cancellation.

### Key Packages (`name.connolly.david.pgs.*`)

| Package | Purpose |
|---------|---------|
| (root) | Core domain: `Timecode`, `FrameRate`, `Resolution`, `SubtitleEvent`, `Render` |
| `.color` | NeuQuant color quantization, YCrCb Rec709 color space |
| `.concurrency` | Pipeline runnables and `EncodeQueue` |
| `.console` | CLI entry point |
| `.ui` | Swing GUI entry point and dialogs |
| `.util` | PES/TS packets, SUP reporting, `ProgressSink` interface |

### Key Design Decisions

- **Render** is an enum singleton managing native library lifecycle
- **Timecode** uses Facebook's "flicks" (1 sec = 705,600,000 flicks) for frame-rate-precise timing without floating point
- **FrameRate** enum: FILM (24), FILM_NTSC (23.976), TV_PAL (25), TV_NTSC (29.97), HD_PAL (50), HD_NTSC (59.94)
- **Resolution** enum: NTSC_480, PAL_576, HD_720, HD_1080
- Java 1.6 target (being modernized) — originally required no diamonds, no try-with-resources, no lambdas

## Entry Points

- GUI: `name.connolly.david.pgs.ui.PunkGraphicStream`
- CLI: `name.connolly.david.pgs.console.PunkGraphicStream`

## Behavioral Guidelines

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

### Think Before Coding
- State assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them — don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

### Simplicity First
- No features beyond what was asked. No abstractions for single-use code.
- No speculative "flexibility" or error handling for impossible scenarios.
- If 200 lines could be 50, rewrite it.

### Surgical Changes
- Don't "improve" adjacent code, comments, or formatting.
- Match existing style, even if you'd do it differently.
- Remove imports/variables that YOUR changes made unused, but don't touch pre-existing dead code.
- Every changed line should trace directly to the user's request.
- When reviewing, only flag issues in OUR code. Never fix, reorder, or restyle pre-existing upstream code — even if it violates conventions. Our patches must be minimal diffs against upstream.

### Goal-Driven Execution
- Transform tasks into verifiable goals with success criteria.
- For multi-step tasks, state a brief plan with verification steps.
- Strong success criteria let you loop independently. Weak criteria ("make it work") require clarification — ask before starting.

### Document Before You Work (MUST)
Before any decision or body of work that changes design, architecture, or approach:
1. Update the relevant `PLAN.md` or `PHASE[N].md` to record the decision and rationale
2. Commit and push those docs immediately, in a separate commit — do not batch with code changes
3. After completing a plan step in code, update the plan to reflect what was actually implemented, then commit and push again
This ensures the record reflects intent, not reconstruction. Skip only for trivial fixes with no design content. Plan files live in the top-level repository only — never inside git submodules.

### Static Builds (MUST)
When producing release binaries, link third-party dependencies statically wherever the host OS allows:
- Build all deps (zlib, freetype, fribidi, harfbuzz, libass, etc.) from source as `.a` files into an isolated sysroot
- Never use system or Homebrew shared libraries for release builds — they will not be present on the user's machine
- Use `--disable-shared --enable-static` for autoconf deps; `-DBUILD_SHARED_LIBS=OFF` for CMake deps
- Isolate pkg-config with `PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"` and `PKG_CONFIG_PATH=""`
- Pass `--pkg-config-flags=--static` and `--extra-cflags/ldflags` pointing at the sysroot to the top-level build
- On Windows/mingw: add `-static-libgcc`. `libwinpthread-1.dll` cannot be eliminated — harfbuzz's CMake detects pthreads via `find_package(Threads)` on non-MSVC Windows and emits `-lpthread` into `harfbuzz.pc`; bundle the DLL in the zip instead
- On Linux: add `-static-libgcc -static-libstdc++` to avoid pulling in `libgcc_s.so` and `libstdc++.so` (harfbuzz brings C++ object files into the final link)
- Only platform-mandated dynamic libs are acceptable (glibc on Linux, libSystem.dylib on macOS, kernel DLLs on Windows)

### Release Versioning
FFmpeg release tags follow the format `n{ffmpeg-version}-pgsN.{build}`:
- `n8.0.1-pgs3.0` — first build of series v3 on the 8.0.1 base
- Increment `.build` for rebuilds on the same series (build config fixes)
- Increment `N` for a new series version
- Do not reuse an existing tag for a retry — delete with `--cleanup-tag` and increment build
- Only bump the tag after a confirmed stable build (all 6 targets green, artifacts verified)
- Run FATE locally before cutting a release

### Branch and Version Discipline (MUST)
Each patch series version gets its own branch. Never commit new-version work on an old-version branch.
- **Branch naming:** `pgs{N}` for version N on master, `pgs{N}-8.0.1` for version N on 8.0.1
- **History tags:** `history/pgs-v{N}` tags the final state of each version
- **New phase = new version:** when starting a new phase of work (new features beyond review fixes), create a new branch (`pgs{N+1}`) from the current HEAD. Do not continue on `pgs{N}`.
- **Freeze old branches:** once `history/pgs-v{N}` is tagged, `pgs{N}` is frozen. Only cherry-pick critical fixes if needed for a release.
- **Before committing:** verify you are on the correct branch for the work being done. `git branch --show-current` should match the version you intend to modify.

### Patch Series Discipline (MUST)
Every patch in a series MUST compile independently when applied in sequence. This is non-negotiable for upstream submission and bisectability.
- **Before committing a patch:** verify that every `#include` references a file that exists at that point in the series — either from upstream or from an earlier patch.
- **Dependency order:** if patch B includes a header created by patch A, patch A MUST come first. Plan the series order BEFORE writing patches. Draw the dependency graph.
- **Two-patch structure:** Patch 1 creates new files (FATE trivially passes), Patch 2 refactors consumer (FATE must be bit-for-bit). The library/header patch always comes before its consumer.
- **Verification:** after building a series, run `git rebase upstream/master --exec 'make -j$(nproc)'` to confirm every patch compiles. This is not optional — do it before pushing.

### FFmpeg Extraction Patterns
- **API boundary validation:** When moving code from a filter (with AVOption validation) into a library API, the library must validate its own inputs. AVOption constraints don't follow the code. Any parameter feeding shifts, array indices, or division needs bounds checking at the API boundary.
- **Heap allocation changes:** When replacing inline struct arrays with heap-allocated contexts, void return types may need to become int to propagate allocation failures. Trace NULL safety through the full call chain.
- **Review discipline:** Three passes (upstream style, security, functional). For each finding, determine whether we introduced it or it's pre-existing. Only fix what we introduced. Document intentional trade-offs in commit messages.
