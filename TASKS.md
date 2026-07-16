# Zeebulator — Task Breakdown

Status: Draft v0.1
Companion to PRD.md (goals/milestones) and ARCHITECTURE.md (component design).
Phases are sequential in intent but some overlap is expected — e.g. Phase 2
research informs Phase 1 finalization. Each phase lists its exit criterion.

---

## Phase 0 — Project Setup
Exit criterion: a contributor can clone, build an empty core, and CI is green.

- [x] Project name decided: **Zeebulator**
- [x] Open-source: confirmed
- [x] License decided: **GPLv3**
- [x] Add `LICENSE` file (GPLv3) to repo root
- [x] Initialize git repo, `.gitignore`, base directory layout (ARCHITECTURE.md §4)
- [x] `CMakeLists.txt` skeleton with `ZEEBULATOR_BUILD_LIBRETRO` / `ZEEBULATOR_BUILD_STANDALONE` options
      — verified building `zeebulator_core`, `zeebulator_standalone`, and
      `zeebulator_libretro` locally
- [x] Set up GitHub Actions CI matrix (Windows/macOS/Ubuntu, both build targets)
- [x] Add `CONTRIBUTING.md` codifying the clean-room policy (PRD §6.3 LR2) — no
      pasting SDK/decompiled source into the repo, ever
- [x] Pull down and locally archive (outside the repo — not committed) the
      research materials: official Zeebo SDK (v0.93, v1.2.4), BREW Developer
      Guide, BREW OEM API Reference for MSM, ARM1136 TRM, from the
      tripleoxygen.net mirror — saved under `research/docs/` (git-ignored)
- [x] Acquire one simple commercial game dump for local dev/test use —
      Double Dragon (`mif`/`mod`/`sig`/`data.ggz`/`sound.ggz`) added under
      `research/games/Double Dragon/` (git-ignored)
- [ ] Acquire a small set of BREW SDK sample apps for local dev/test use —
      go in `research/samples/` (git-ignored) — still pending
- [x] Set up test infrastructure: GoogleTest via CMake `FetchContent`, wired
      through CTest (`ZEEBULATOR_BUILD_TESTS` option) — verified building
      and passing (`tests/core_test.cpp` smoke test green) locally

## Phase 1 — CPU & Memory Core
Exit criterion: can load an arbitrary ARM binary blob into emulated memory
and execute it correctly, verified against ISA test vectors — no BREW
awareness yet.

- [ ] Stand up Memory Subsystem: flat 32-bit address space, typed
      read/write, bulk-load API (ARCHITECTURE.md §3.2)
- [ ] Implement `IArmCore` interface (ARCHITECTURE.md §3.1)
- [ ] Implement v1 ARMv6 interpreter covering the ARM1136J-S instruction set
- [ ] Unit tests against known ARMv6 instruction-behavior vectors
- [ ] **Research task**: determine the actual BREW call-out mechanism (how
      app code invokes AEE interface methods — vtable call to a
      real/fake address vs. SWI/trap instruction). Cross-reference the BREW
      OEM API Reference and Developer Guide; if inconclusive, disassemble a
      BREW sample app's compiled output to observe the calling convention
      directly. This determines the CPU core's hook API — do not proceed to
      Phase 3 until resolved.
- [ ] Add CPU core hook points for trapping on the call-out mechanism found above
- [ ] Evaluate `dynarmic` integration spike (does it support the exact
      ARMv6/ARM1136 feature set needed, license fit, build complexity) —
      decide interpreter-only-for-now vs. JIT-from-the-start

## Phase 2 — Loader (GGZ / BAR / MIF / .mod)
Exit criterion: a real game's GGZ archive can be opened and its `.mod` code
mapped into memory with a valid entry point, ready to execute (even though
it will crash immediately without Phase 3).

- [ ] Clean-room GGZ archive reader (gzip-based container) — reference
      `ggzbrewtools`' *documented format behavior*, not its source
      (PRD §6.3 LR2)
- [ ] BAR file parser
- [ ] MIF (Module Information File) parser — entry points, resource references
- [ ] `.mod` loader: code/data segment mapping, relocation, initial register
      state per BREW's documented ARM entry convention
- [ ] Dev tool: standalone GGZ inspector/extractor CLI (`tools/`) for
      debugging content during development
- [ ] Test fixtures: loader unit tests against SDK sample apps

## Phase 3 — Minimal BREW HLE (bring-up target: blank painted screen)
Exit criterion: **M0 from PRD §7** — a BREW "hello world" sample app boots
via the standalone frontend and reaches a visible, correctly-painted screen.

- [ ] Stand up Backend Abstraction Interface (ARCHITECTURE.md §3.8)
- [ ] Minimal standalone SDL2 frontend implementing that interface
      (window + framebuffer blit only, no audio/input yet)
- [ ] Implement `IShell` HLE: app lifecycle (init, notify, suspend/resume
      stubs) — just enough for an app to start and stay alive
- [ ] Implement minimal `IDisplay` HLE: framebuffer creation, basic 2D
      blit/update-screen calls
- [ ] Wire CPU core call-out traps (Phase 1) to the `IShell`/`IDisplay` HLE dispatch
- [ ] **Milestone M0 checkpoint**: boot the simplest SDK sample app to a
      visible screen. Do not proceed to Phase 4 until this works —
      it's the smallest possible end-to-end validation of the whole pipeline.

## Phase 4 — File System & Asset Access
Exit criterion: a game can enumerate and read its own bundled assets
through HLE `IFile` calls, sourced from the loaded GGZ contents.

- [ ] Implement `IFile`/`IFileMgr` HLE backed by an in-memory virtual
      filesystem populated from the loaded GGZ archive (never expose the
      real host filesystem directly — ARCHITECTURE.md §3.4)
- [ ] Handle whatever asset sub-formats appear inside GGZ containers for the
      target test game (models, sprites, etc. — format specifics are a
      per-content research task, informed by `ggzbrewtools`' documented
      coverage)

## Phase 5 — Graphics (OpenGL ES 1.0/1.1 translation)
Exit criterion: the target test game's 3D/2D rendering appears on screen,
even if visually imperfect.

- [ ] Design the GLES1.1 state-machine translation layer (ARCHITECTURE.md §3.5)
- [ ] Decide fixed-function-passthrough vs. shader-generation approach per
      platform (macOS forces the latter — architecture §10 risk)
- [ ] Implement matrix stack, basic lighting, texture combiner translation
- [ ] Wire into `IDisplay`/graphics-related AEE calls
- [ ] Validate against SDK sample apps that exercise GLES before touching
      the target commercial game

## Phase 6 — Audio
Exit criterion: target test game's audio plays back correctly.

- [ ] PCM decode/playback path
- [ ] IMA-ADPCM decoder
- [ ] MIDI playback
- [ ] MP3 decoder integration
- [ ] Mixer + ring buffer feeding the Backend Abstraction Interface
- [ ] Implement `IMedia`/`ISound` HLE wired to the above

## Phase 7 — Input
Exit criterion: target test game responds correctly to controller input.

- [ ] Implement `IHID` HLE (Zeebo Z-Pad extension) — ARCHITECTURE.md §3.7
- [ ] Standalone frontend: SDL2 gamepad/keyboard → `ZPadState` mapping
- [ ] Default input mapping matching a standard Xbox-layout controller

## Phase 8 — First Playable Commercial Game
Exit criterion: **M1 from PRD §7** — target title (Double Dragon) fully
playable start-to-finish at full speed, standalone build.

- [ ] Iteratively debug against the real game, filling HLE API gaps as they're hit
- [ ] Add any needed per-title quirks to `core/brew/compat/`, keyed by game
      hash — never inline in general HLE code (Design Principle 5)
- [ ] Lock in this title as a permanent CI regression fixture once it passes
      (ARCHITECTURE.md §8)
- [ ] Performance pass: if the interpreter can't hold full speed, this is
      the point to seriously commit to the JIT integration deferred in Phase 1

## Phase 9 — Libretro Core
Exit criterion: **M2 from PRD §7** — same game fully playable through the
libretro core in RetroArch, with working save states.

- [ ] Implement libretro core shim (ARCHITECTURE.md §3.9): `retro_init`,
      `retro_deinit`, `retro_get_system_info`, `retro_get_system_av_info`,
      `retro_load_game`, `retro_run`, `retro_reset`
- [ ] Wire video via `retro_hw_render_callback` (OpenGL)
- [ ] Wire audio via `retro_audio_sample_batch_t`
- [ ] Wire input via `retro_input_poll_t`/`retro_input_state_t` → `ZPadState`
- [ ] Implement `retro_serialize`/`retro_unserialize` (save states — full
      memory + register snapshot; verify no AEE interface holds
      un-serialized state, e.g. open virtual file handles)
- [ ] Author the core `.info` file (name, extensions, **no required
      firmware** — PRD §3.2 / ARCHITECTURE §6)
- [ ] Add core options (`retro_variable`): interpreter/JIT toggle, input
      mapping presets, aspect ratio
- [ ] Manual test in actual RetroArch (not just libretro-compatible test harnesses)

## Phase 10 — Packaging & Distribution
Exit criterion: **M4 from PRD §7** — installable/runnable builds exist for
all three OSes plus the libretro core.

- [ ] CMake install targets for standalone builds (Windows/macOS/Linux)
- [ ] macOS notarization/signing (if pursuing outside-Gatekeeper distribution)
- [ ] Windows code signing (optional, cost/benefit TBD)
- [ ] GitHub Actions release workflow producing artifacts for all targets
- [ ] Package the libretro core per libretro-super/buildbot conventions;
      evaluate submitting for official RetroArch core listing
- [ ] Write end-user docs: how to legally obtain/dump games, how to load
      them, known compatibility list

## Phase 11 — Compatibility Growth (ongoing)
Exit criterion: **M3 from PRD §7** — 5-10 playable titles; open-ended past that.

- [ ] Public, maintained compatibility list (docs/ or repo wiki)
- [ ] Triage process for community bug reports on new titles
- [ ] Expand `compat/` quirks database as new titles are brought up
- [ ] Revisit JIT performance work if new titles expose interpreter bottlenecks

---

## Cross-cutting / Not Phase-Bound

- [ ] Keep `CONTRIBUTING.md`'s clean-room policy enforced via PR review —
      this is a legal-risk item, not just a style preference (PRD §6.3)
- [ ] Revisit license decision if the project seeks libretro official listing
      (buildbot may have its own requirements to check)
- [ ] Every task/unit of work lands with tests covering it; run the full
      suite (`ctest --test-dir build`) before considering that task done —
      standing project convention (ARCHITECTURE.md §8), not a one-time setup item
