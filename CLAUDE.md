# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

PunkGraphicStream (v0.7.2) converts ASS (Advanced SubStation Alpha) subtitle files into PGS/SUP (Presentation Graphic Stream) subtitle files for Blu-ray discs. It is a Java 1.6 application with a native C library (libpgs-jni) wrapping libass via JNI.

## Repository Structure

Parent project with three git submodules:
- `punkgraphicstream/` - Main Java application (Maven project)
- `libpgs-jni/` - Native C JNI wrapper around libass
- `libass/` - libass library (pinned to 0.17.4)

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

### Simplicity First
- No features beyond what was asked. No abstractions for single-use code.
- No speculative "flexibility" or error handling for impossible scenarios.
- If 200 lines could be 50, rewrite it.

### Surgical Changes
- Don't "improve" adjacent code, comments, or formatting.
- Match existing style, even if you'd do it differently.
- Remove imports/variables that YOUR changes made unused, but don't touch pre-existing dead code.
- Every changed line should trace directly to the user's request.

### Goal-Driven Execution
- Transform tasks into verifiable goals with success criteria.
- For multi-step tasks, state a brief plan with verification steps.
