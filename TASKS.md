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
- [x] Acquire a small set of BREW SDK sample apps for local dev/test use —
      turned out to already be bundled inside `ZeeboSDKPackage-1.2.4.zip`
      (`samples.zip`), extracted to `research/samples/` (git-ignored):
      ~10 OpenGL ES sample apps in **source form** (`MSM7500_OGLES_...`)
      plus `conftest_source/` (includes `AEEModGen.c`/`AEEAppGen.c`, the
      reference module/applet boilerplate). Source form is strictly better
      for research than a bare `.mod` would have been — see Phase 1 note
- [x] Set up test infrastructure: GoogleTest via CMake `FetchContent`, wired
      through CTest (`ZEEBULATOR_BUILD_TESTS` option) — verified building
      and passing (`tests/core_test.cpp` smoke test green) locally

## Phase 1 — CPU & Memory Core
Exit criterion: can load an arbitrary ARM binary blob into emulated memory
and execute it correctly, verified against ISA test vectors — no BREW
awareness yet.

- [x] Stand up Memory Subsystem: flat 32-bit address space, typed
      read/write, bulk-load API (ARCHITECTURE.md §3.2) — paged/sparse
      backing store, little-endian, `core/memory/`
- [x] Implement `IArmCore` interface (ARCHITECTURE.md §3.1) — `core/cpu/arm_core.h`
- [x] Implement v1 ARMv6 interpreter — `core/cpu/arm_interpreter.cpp`.
      **Honest scope, not full ISA coverage:**
      - Covered: all 16 data-processing opcodes (AND/EOR/SUB/RSB/ADD/ADC/
        SBC/RSC/TST/TEQ/CMP/CMN/ORR/MOV/BIC/MVN), both operand2 forms
        (immediate, register with immediate or register-specified shift,
        all 4 shift types incl. RRX), all 15 condition codes, N/Z/C/V flag
        computation, B/BL, single word/byte LDR/STR (immediate and
        register offset, pre/post-index, writeback), block data transfer
        (LDM/STM, all 4 addressing modes, writeback — added during Phase 2
        real-`.mod` probing, see below), halfword/signed transfer
        (STRH/LDRH/LDRSB/LDRSH — added during Phase 3, hit by our own
        compiled test app, see below), BX/BLX (branch-and-exchange,
        register form, as long as the target stays in ARM state), the
        PC-reads-as+8 operand semantics, and the call-out trap hook.
      - **Deferred, and explicitly rejected via `UnimplementedInstruction`
        rather than silently mis-executed:** multiply/MLA/long-multiply,
        swap (SWP/SWPB), MRS/MSR (PSR transfer), SWI, coprocessor
        instructions, LDM/STM's user-bank/exception-return (S=1) variant,
        BX/BLX to a Thumb target, Thumb state entirely. Will be picked up
        incrementally as real game code needs them — multiply is the next
        highest-value target (real compiled code will need it constantly;
        our own test app hasn't needed it yet, but a real game will).
- [x] Unit tests against known ARMv6 instruction-behavior vectors —
      `tests/cpu_test.cpp`, 27 tests, all hand-encoded real ARM instruction
      words (not synthetic/fake encodings), verified bit-field-by-bit-field
- [x] **Research task, mostly resolved** via `research/samples/` source
      (see Phase 0): BREW's call-out mechanism is plain **C vtable/interface
      calls**, not an SWI/trap instruction. Confirmed from source:
      - A `.mod` exports a well-known entry point (`AEEMod_Load`, per
        `conftest_source/conftest/AEEModGen.c`'s "sample IModule
        implementation") that the loader calls, passing in an `IShell*`.
      - `AEEMod_Load` → `AEEMod_CreateInstance` → the app's own
        `AEEClsCreateInstance(ClsId, pIShell, po, ppObj)` (confirmed in
        `simple_drawtexture.c`), which calls `AEEApplet_New(...)` to
        register an `AEEHANDLER` event callback (`HandleEvent`).
      - The OS then drives the app purely through that event callback,
        starting with `EVT_APP_START`.
      - Every AEE interface (`IShell`, `IDisplay`, `IFile`, ...) is a
        struct of function pointers (COM-style), invoked through macros
        (e.g. `ISHELL_xxx(pIShell, ...)`) — this validates the
        ARCHITECTURE.md §3.4 design as-is: our loader constructs these
        interface objects with vtable slots pointing at sentinel addresses;
        the CPU core traps execution reaching one of those addresses and
        dispatches to HLE.
      - **Still open** (narrower now, not a fundamentally unknown
        mechanism): exact per-interface vtable slot ordering and AAPCS
        register-passing details — work this out per-interface against the
        BREW OEM API Reference as each one gets implemented (Phase 3+),
        not a blocking Phase 1 unknown anymore.
- [x] Add CPU core hook points for trapping on the call-out mechanism found
      above — `SetCallOutRange`/`SetCallOutHandler` on `IArmCore`; `Step()`
      checks the trap range before fetch/decode, `Run()` stops early on a
      trap. Covered by `Cpu.CallOutRangeTrapsInsteadOfExecuting` and
      `Cpu.RunStopsEarlyOnCallOutTrap`.
- [ ] Evaluate `dynarmic` integration spike (does it support the exact
      ARMv6/ARM1136 feature set needed, license fit, build complexity) —
      decide interpreter-only-for-now vs. JIT-from-the-start — not started;
      the v1 interpreter above is correctness-first per Design Principle 4,
      revisit once something is actually slow

## Phase 2 — Loader (GGZ / BAR / MIF / .mod)
Exit criterion: a real game's GGZ archive can be opened and its `.mod` code
mapped into memory with a valid entry point, ready to execute (even though
it will crash immediately without Phase 3).

**Exit criterion met**, and then some — verified with actual instruction
execution, not just "it loads": GGZ opens (89/89 + 74/74 real entries
extracted correctly), `.mod` maps into memory with a valid entry point and
genuinely executes real code correctly (23 real instructions, see below).
It won't get further than that without Phase 3 (BREW HLE), exactly as
expected. BAR is unconfirmed-needed for this title; MIF's full structure
remains deliberately deferred (see below) — neither blocks this phase's
actual goal.

- [x] **Clean-room GGZ archive reader — fully solved and verified.**
      `core/loader/ggz.{h,cpp}`. No public spec exists anywhere (checked:
      official docs, `ggzbrewtools`' README/wiki/issues — read for prose
      only, never its source, per PRD §6.3 LR2 — and general web search;
      all came up empty). Reverse-engineered from scratch by cross-testing
      hypotheses against the real Double Dragon `data.ggz`/`sound.ggz`
      (research-only, never committed) using a scratch Python script, then
      reimplemented independently in C++:
      - File opens with a table of N big-endian 8-byte entries:
        `(offset: u32, decompressed_size: u32)`.
      - The table's own byte length equals the *first* entry's offset
        value (i.e. N = that value / 8) — data starts exactly where the
        table ends.
      - Each entry points to a standard RFC 1952 gzip stream; the asset's
        original filename comes from the gzip header's `FNAME` field when
        present (confirmed: 100% of entries in both real files have it
        set), falling back to an ordinal name otherwise per
        `ggzbrewtools`' documented observation that this can happen.
      - **Verified two ways**: (1) synthetic unit tests
        (`tests/ggz_test.cpp`, 6 tests, self-generated non-copyrighted
        gzip archives), and (2) the real Double Dragon files via
        `zeebulator_ggz_inspector` — **89/89 entries in `data.ggz` and
        74/74 in `sound.ggz` extracted correctly**, byte-exact against an
        independent Python/zlib cross-check. High confidence, not a guess.
      - Contained asset formats (`.obm1` sprites/models, `.wav`/`.mid`
        audio) are NOT parsed yet — GGZ just gets you the named raw
        bytes; understanding `.obm1` internals is graphics-subsystem work
        for a later phase.
- [x] Dev tool: `tools/ggz_inspector.cpp` — lists and optionally extracts
      a GGZ archive's contents from a runtime-supplied path (never embeds
      game content into the repo).
- [ ] BAR file parser — not started. Worth noting: Double Dragon's dump
      doesn't contain any `.bar` file at all (just `mif/`, `mod/`) — it
      may not be universally needed per-title. Revisit if/when a title
      that actually has one shows up.
- [x] MIF (Module Information File) — **string metadata extraction solved
      and shipped; full binary structure deliberately deferred (scope
      decision, not a dead end).** No byte-level spec exists publicly for
      either part (confirmed via web research).
      - **What's shipped**: `core/loader/mif.{h,cpp}` +
        `tools/mif_inspector.cpp`. MIF embeds human-readable app metadata
        as UTF-16LE strings, each prefixed with an `0xFFFE` BOM marker,
        ending at a null code unit or (since strings can sit back-to-back
        with no separator) the next BOM. Extracting these directly is
        enough for a game-library UI (name/publisher/version) without
        needing the surrounding binary format solved. Verified against
        all 11 available SDK sample `.mif` files (each yields its own
        description string, e.g. `"simple_atitc - Texture Compression"`)
        and the real Double Dragon `.mif` (yields `"DOUBLE DRAGON
        Zeebo"`, `"Brizo Interactive Corp."`, `"0.9.0"` — title,
        publisher, version). A strict printable-only filter correctly
        rejects a coincidental BOM-byte-pattern false-positive found
        inside binary (non-string) data elsewhere in the real file, and
        a string with a trailing non-printable code unit — conservative
        by design, since this is for UI display. 13 unit tests
        (`tests/mif_test.cpp`, synthetic buffers) + verified against all
        12 real files via `zeebulator_mif_inspector`.
      - **What's still open** (deferred — not needed for loading/running
        a specific game, only for full BREW-style app management, which
        this project doesn't need — see conversation record for the
        scoping reasoning): the resource table, class ID, and privilege
        bits. Partial header structure was reverse-engineered during this
        pass (not yet implemented in code, recorded here so it isn't
        lost): fixed 32-byte header — word0 = constant magic `0x00010011`;
        word1's low 16 bits are always 1, high 16 bits are a count
        (`count_A`); word2 = 32 (constant, doubles as the offset where a
        table starts); word3 = `count_A * 8` (that table's byte length,
        confirmed across all 4 files checked); word4 = word2 + word3
        (offset where that table ends); word5 = another count
        (`count_B`). What word6 onward means did NOT resolve cleanly
        (an initial guess that it pointed at the string table matched for
        one file and not others) — this is genuinely unresolved, not
        glossed over. A real, sourced, untested lead for interpreting the
        table entries themselves: `AEEShell.h`'s resource-ID offset scheme
        (`IDR_NAME_OFFSET=0, IDR_ICON_OFFSET=1, IDR_IMAGE_OFFSET=2,
        IDR_THUMB_OFFSET=3, IDR_SETTINGS_OFFSET=4, IDR_VERSION_OFFSET=5,
        IDR_ENVIRONMENT_OFFSET=6, IDR_OFFSET_STEP=20` — note `20` and `26`
        showed up in exactly this relationship in one file's table
        records, which may not be coincidence). Also noted: at least one
        known BREW/Zeebo title's MIF is reportedly encrypted (per a
        Tuxality devlog), so not every MIF may be plain-structured at
        all. If a real need for the class ID emerges during `.mod`
        loading, check first whether it's cheaper to get it another way
        (e.g. Double Dragon's `.mif` is literally named `274754.mif` —
        that number is a plausible class ID without parsing anything)
        before resuming this.
- [x] `.mod` loader — **solved and verified against real code execution,
      not just static analysis.** Research suggested "self-relocatable"
      (implying a relocation table); that turned out to be misleading —
      the real answer is simpler. `core/loader/mod.{h,cpp}`:
      - **Finding**: `.mod` is a flat, headerless, position-independent
        ARM binary. Raw code+data starts at file offset 0. No header, no
        relocation table, loadable at any base address.
      - **How this was verified** (`tools/mod_probe.cpp`, a dev tool that
        loads a raw `.mod` at a chosen address and single-steps the real
        interpreter through it, printing each instruction): loaded the
        real Double Dragon `ddragonz.mod` at an arbitrary address
        (`0x00100000`, chosen for no particular reason — the whole point
        was to confirm the code doesn't care) and executed it. Result:
        **23 real instructions executed correctly**, decoding as
        completely coherent, idiomatic compiler-generated ARM: a function
        prologue (`STR LR,[SP,#-4]!` / `SUB SP,SP,#12`), sensible
        register shuffling, a `BL` that correctly landed on a *second*
        function's prologue (`STMFD SP!, {r3-r9,lr}`) at a totally
        different file offset, that function's body and matching epilogue
        (`LDMFD SP!, {r3-r9,lr}` popping exactly what was pushed), and two
        correct `BX LR` returns — all with register values propagating
        exactly as expected at every step. This is about as strong a
        real-execution confirmation as is possible without the actual
        BREW OS driving it. (Execution eventually "returns" to address 0
        and starts executing harmless zeroed-memory no-ops — expected and
        correct, not a bug: our probe doesn't yet set up the real initial
        LR/SP the OS would provide before calling `AEEMod_Load`, which is
        Phase 3 work, not a `.mod`-format problem.)
      - **Direct payoff for Phase 1**: probing this real code is what
        motivated (and validated) implementing LDM/STM and BX in the CPU
        interpreter just now — both were hit almost immediately by real
        compiled code, confirming they were exactly the right next
        instructions to add.
      - `elf2mod.x`/relocation-table research from before this session is
        now believed to be a red herring for the common case (plain
        position-independent code) — kept in git history rather than
        deleted from here, in case some other title's `.mod` does need
        real relocation (not every game need be compiled identically).
      - `LoadMod()` is intentionally thin (load bytes, set PC) since
        there's no header to parse. 3 unit tests (`tests/mod_test.cpp`),
        including one that specifically verifies the *same* image produces
        identical behavior at two different base addresses — the actual
        position-independence property, not just "it loads."
      - The `.sig` file alongside each `.mod` (e.g. `ddragonz.sig`) is
        almost certainly a cryptographic signature for real-device code
        verification — irrelevant to an HLE emulator with no real
        security boundary to enforce, so deliberately ignored.
- [x] Test fixtures: **deviated from the original plan, in a good way.**
      SDK sample apps don't include any `.ggz` files at all (only Double
      Dragon's commercial dump does), so GGZ and MIF tests both use
      self-generated synthetic fixtures instead (`tests/ggz_test.cpp`,
      `tests/mif_test.cpp`) — real-file correctness for both was
      additionally confirmed via `zeebulator_ggz_inspector` /
      `zeebulator_mif_inspector` against the local, git-ignored real
      files (Double Dragon's dump + all 11 SDK sample `.mif`s), giving
      two independent layers of verification without committing any
      copyrighted content. Same pattern should carry forward to `.mod`.

## Phase 3 — Minimal BREW HLE (bring-up target: blank painted screen)
Exit criterion: **M0 from PRD §7** — a BREW "hello world" sample app boots
via the standalone frontend and reaches a visible, correctly-painted screen.

**Core HLE pipeline is done and verified end to end; the one remaining
piece is an actual SDL2 window** (currently proven via a test double that
captures the exact pixel output instead of a real window — see below).

- [x] Stand up Backend Abstraction Interface (ARCHITECTURE.md §3.8) —
      done back in Phase 0 (`core/backend.h`): `PushVideoFrame`,
      `PushAudioSamples`, `PollInput`.
- [ ] Minimal standalone SDL2 frontend implementing that interface
      (window + framebuffer blit only, no audio/input yet) — **not
      started.** Everything up to this point has been verified against a
      `CapturingBackend` test double (`tests/brew_lifecycle_test.cpp`)
      that asserts on exact pixel values instead of a real window; wiring
      an actual SDL2-backed `Backend` implementation is the one piece
      standing between what's proven now and a human seeing a window.
- [x] Implement `IShell` HLE (`core/brew/ishell.cpp`) — vtable slot order
      verified directly against real Qualcomm source (`AEEIShell.h`, see
      below). All 42 pre-BREW-MP slots present; every slot is currently a
      safe stub (nothing in scope calls any IShell method yet — the test
      app gets `IDisplay` directly via the `EVT_APP_START` event, not
      through IShell). Extend individual slots with real behavior as
      games need them.
- [x] Implement `IDisplay` HLE (`core/brew/idisplay.cpp`) — vtable slot
      order verified directly against real `AEEIDisplay.h`. Real behavior
      for `DrawText` (draws a placeholder block sized from the real
      x/y/length arguments — no font rasterizer yet, that's later
      graphics-phase work) and `Update` (pushes the framebuffer to
      `Backend::PushVideoFrame`); the other 11 slots are stubs.
- [x] Wire CPU core call-out traps (Phase 1) to the `IShell`/`IDisplay` HLE
      dispatch — `core/brew/hle_runtime.{h,cpp}` (`HleRuntime`). Also
      provides the reverse direction, `CallArmFunction()`: calling INTO
      the app's own compiled code and running until it returns, which
      turned out to be necessary (see below) — not originally scoped as
      part of "wire call-out traps" but required to actually drive an
      app's lifecycle rather than just service its calls.
- [x] **Real Qualcomm vtable ABI, verified from actual source, not
      guessed.** No public byte-level IShell/IDisplay spec exists (same
      story as every other format in this project), but unlike
      MIF/`.mod`, the *header declarations* (not implementations) for
      these interfaces turned out to be findable: archive.org hosts full
      original BREW SDK installers (`brew_1.1_sdk` from ~2001,
      `bmp-sdkmp-7.12.5` from 2012). Extracted both (outside the repo,
      git-ignored, `research/docs/sdk_installer_extract/
      brew_sdk_headers_reference/`) and independently read the real
      `AEEIShell.h`/`AEEIDisplay.h` vtable macros directly (not just
      trusted a research agent's summary — first verification pass
      actually returned zero matches due to the files' non-standard
      ASCII encoding silently breaking `grep`'s locale handling; re-ran
      with `LC_ALL=C` and confirmed for real). Slot order is identical
      across both SDK versions (BREW's ABI policy is strictly
      append-only), which is why the pre-BREW-MP subset is trustworthy
      for a 2009-era Zeebo target even though only 2001 and 2012 were
      directly checked.
- [x] **Full app lifecycle actually driven end to end, against real
      compiled ARM code** (`tests/brew_lifecycle_test.cpp`,
      `tests/fixtures/hello_brew/`). Wrote our own minimal BREW-shaped
      test app in C (not Qualcomm code — see the file's header comment),
      structurally faithful to the real `AEEMod_Load` ->
      `IModule::CreateInstance` -> `HandleEvent(EVT_APP_START)` contract
      reverse-engineered from the official `AEEModGen.c`/`AEEAppGen.c`
      reference sources, compiled with `arm-none-eabi-gcc` (real
      cross-compiler, not `elf2mod.exe`) targeting the same
      `-march=armv5te -mthumb-interwork` flags Zeebo used. Result,
      verified pixel-exact: our loader calls the compiled `AEEMod_Load`,
      reads back the returned `IModule*`, calls its `CreateInstance`
      through the vtable, gets back a `HandleEvent` function pointer,
      builds a real `AEEAppStart` struct in emulated memory, calls
      `HandleEvent(EVT_APP_START, ...)` — and the app's own compiled code
      correctly calls `IDISPLAY_DrawText` then `IDISPLAY_Update` through
      the vtable we built, landing exactly the expected pixels in the
      framebuffer.
  - Compiling this test app surfaced two genuinely necessary CPU core
    additions (not speculative — both were hit immediately by real
    compiled code): **BLX** (register-form branch-and-exchange-with-link
    — real vtable calls use this, not a bare `BX`) and **halfword/signed
    transfer** (`STRH`/`LDRH`/`LDRSB`/`LDRSH` — real compiled code uses
    these for 16-bit locals). Both implemented with real tests, not just
    added to make the integration test pass.
  - Compiling it also caught a **real dispatcher bug**: the "opcode
    0x8-0xB with S=0 is reassigned to the miscellaneous instruction space
    (MRS/MSR/BX/...)" rule was being checked *before* the
    multiply/halfword bit-pattern check, but bits[24:21] only mean
    "opcode" for true data-processing-shaped instructions — `STRH`'s
    P/U/I/W encoding bits happen to numerically collide with that opcode
    range, so it was being wrongly routed to "unimplemented misc
    instruction" instead of halfword transfer. Fixed by checking the
    multiply/halfword bit pattern first; would not have been caught
    without testing against real compiled code, since none of the
    hand-encoded unit tests happened to exercise that exact collision.
- [ ] **Milestone M0 checkpoint**: not yet fully met — the HLE
      pipeline and app lifecycle are proven correct against a captured
      framebuffer, but nothing has been shown in an actual window yet.
      SDL2 frontend integration is the remaining step.

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
