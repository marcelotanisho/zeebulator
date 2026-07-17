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
        PC-reads-as+8 operand semantics, the call-out trap hook, and (added
        after Phase 6, closing the longest-standing deferred item) multiply/
        multiply-accumulate: `MUL`/`MLA` and long multiply `UMULL`/`UMLAL`/
        `SMULL`/`SMLAL`. Correctly propagates carry from the low to the high
        32-bit word during 64-bit accumulation (verified with a dedicated
        test — this is exactly the kind of thing a naive "two separate
        32-bit adds" implementation gets wrong) and sets N/Z from the full
        64-bit result for the long forms; C/V are left unchanged for S=1,
        matching the ARM ARM's own "UNPREDICTABLE" carry-flag behavior for
        these opcodes rather than inventing a specific value. 8 new tests
        (`tests/cpu_test.cpp`), every hand-encoded instruction word
        independently generated and cross-checked via a small Python
        encoder rather than computed by hand, given how easy nibble-level
        arithmetic is to get subtly wrong (a lesson already learned once in
        Phase 1's original `BIC` test).
        **Verified against real Double Dragon code, honestly**: reran
        `zeebulator_mod_probe` against the real `ddragonz.mod` — execution
        still stops at exactly the same 23rd instruction as before (falls
        into the same documented "returns to address 0, executes harmless
        zeroed no-ops" behavior). That's expected, not a sign multiply
        didn't help: this probe only exercises the module's tiny entry
        stub (`AEEMod_Load` and a couple of helper calls), which never
        needed multiplication — real usage will only show up once Phase 8
        drives the actual game logic through a real, fuller BREW lifecycle
        (real `IShell` behavior, real `ClsId`, real `EVT_APP_START` value,
        none of which exist yet). Implementing this now removes a known,
        named blocker ahead of that work rather than proving a payoff that
        genuinely can't be observed yet at this shallow a depth.
      - **Still deferred, and explicitly rejected via
        `UnimplementedInstruction` rather than silently mis-executed:**
        swap (SWP/SWPB), MRS/MSR (PSR transfer), SWI, coprocessor
        instructions, LDM/STM's user-bank/exception-return (S=1) variant,
        BX/BLX to a Thumb target, Thumb state entirely. Will be picked up
        incrementally as real game code needs them.
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

**Phase 3 is complete — M0 achieved and visually confirmed**, not just
proven via a test double. Screenshot-verified: a real SDL2 window titled
"Zeebulator" opens, shows a black 640x480 canvas (Zeebo's native
resolution) with a white block visibly drawn where `hello_brew`'s
`HandleEvent` called `IDISPLAY_DrawText`/`IDISPLAY_Update` through the
real vtable.

- [x] Stand up Backend Abstraction Interface (ARCHITECTURE.md §3.8) —
      done back in Phase 0 (`core/backend.h`): `PushVideoFrame`,
      `PushAudioSamples`, `PollInput`.
- [x] Minimal standalone SDL2 frontend implementing that interface
      (`frontends/standalone/main.cpp`) — window + framebuffer blit via a
      streaming `SDL_Texture` (RGB565, matching `IDisplayHle`'s
      framebuffer format directly, no conversion needed); no audio/input
      yet (`Sdl2Backend::PushAudioSamples`/`PollInput` are no-ops for
      now). SDL2 vendored via CMake `FetchContent` (`libsdl-org/SDL`,
      `release-2.30.9`) consistent with the project's existing
      zlib/GoogleTest pattern, built as a static lib
      (`SDL2::SDL2`/`SDL2::SDL2main`). On Linux this needed X11 dev
      headers installed on the build machine (`libx11-dev` and friends —
      build-time only, never needed by end users or by anyone just
      running a released binary).
      Currently boots the bundled `hello_brew` M0 demo specifically
      (hardcoded fixture path with an optional CLI override) — loading
      arbitrary real games needs the `.mod` entry-point-discovery and
      GGZ/MIF wiring that's later-phase work; this frontend exists right
      now to prove the pipeline, not as the final game loader.
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
- [x] **Milestone M0 checkpoint: achieved, screenshot-verified.** Ran
      `zeebulator_standalone`, captured the actual window with
      `gnome-screenshot`, and visually confirmed a black 640x480 canvas
      with a white block drawn at the expected position — the smallest
      possible end-to-end validation of the whole pipeline (CPU
      interpreter -> HLE dispatch -> real compiled ARM code -> vtable
      calls -> framebuffer -> SDL2 window), and it holds up.

## Phase 4 — File System & Asset Access
Exit criterion: a game can enumerate and read its own bundled assets
through HLE `IFile` calls, sourced from the loaded GGZ contents.

**Exit criterion met.** A game can open, read (with correct partial-read
and independent-multi-handle semantics), seek, get info on, and enumerate
its own GGZ-backed assets through real, vtable-order-verified `IFile`/
`IFileMgr` HLE calls.

- [x] Implement `IFile`/`IFileMgr` HLE backed by an in-memory virtual
      filesystem populated from the loaded GGZ archive (never expose the
      real host filesystem directly — ARCHITECTURE.md §3.4).
      `core/brew/virtual_filesystem.{h,cpp}` (`VirtualFilesystem`, flat
      name -> bytes map, eager decompression via `GgzArchive::Extract()`
      — already real-file-verified in Phase 2, so no redundant real-data
      check was needed here) + `core/brew/file_hle.{h,cpp}` (`FileHle`,
      the actual HLE dispatch).
      **Real Qualcomm vtable ABI, verified from actual source, same
      method as IShell/IDisplay** (archive.org-hosted original BREW SDK
      installers — see Phase 3 for how that source was first found).
      This round needed a fresh background research pass (the specific
      headers weren't part of what got extracted for Phase 3), which hit
      a genuine large-file download (892MB BREW MP 7.12.5 installer) that
      looked stalled at first glance — checked the actual transcript
      instead of trusting a vague status update, confirmed real transfer
      progress (~1.7MB/s, growing) via a timed size-delta check, and let
      it finish properly rather than assuming failure. Independently
      re-verified the resulting header content myself afterward (same
      `LC_ALL=C` lesson from Phase 3 — plain `grep` silently misses
      content in these ISO-8859/non-UTF8-encoded files).
      Both `IFile` and `IFileMgr` are defined together in a single
      `AEEFile.h` in both SDK versions (no separate `AEEFileMgr.h`
      exists) — confirmed vtable orders:
      - `IFile` (inherits `IAStream`): `AddRef, Release, Readable, Read,
        Cancel`, then `Write, GetInfo, Seek, Truncate` (BREW MP adds
        `GetInfoEx, SetCacheSize, Map` — not implemented, post-dates
        Zeebo).
      - `IFileMgr`: `AddRef, Release, OpenFile, GetInfo, Remove, MkDir,
        RmDir, Test, GetFreeSpace, GetLastError, EnumInit, EnumNext,
        Rename` (BREW MP adds 8 more trailing slots — not implemented).
      - Confirmed append-only/stable across both 2001 and 2012 SDK
        versions, same pattern as IShell/IDisplay.
      Files are read-only (`Write`/`Remove`/`MkDir`/`RmDir`/`Truncate`/
      `Rename` all return an error rather than silently succeeding or
      doing nothing) since GGZ contents aren't meant to be mutated. One
      real design point worth noting: `IFile` instances share a single
      vtable (built once) but each `OpenFile` call allocates a fresh
      object header at a bump-allocated address, with per-handle state
      (current read position, which file) looked up by that address at
      dispatch time — this is what makes multiple simultaneously-open
      files with independent read positions work correctly, verified by
      a dedicated test.
      17 new tests (`tests/virtual_filesystem_test.cpp`,
      `tests/file_hle_test.cpp`), all synthetic (no copyrighted content).
- [ ] Handle whatever asset sub-formats appear inside GGZ containers for the
      target test game (models, sprites, etc. — format specifics are a
      per-content research task, informed by `ggzbrewtools`' documented
      coverage) — **deferred, not blocking this phase's exit criterion.**
      `IFile`/`IFileMgr` hand back raw bytes correctly regardless of what
      those bytes mean internally; understanding `.obm1` sprite/model
      internals is graphics-subsystem work (Phase 5) triggered by
      whatever a real game's rendering code actually needs, not a
      file-access concern.

## Phase 5 — Graphics (OpenGL ES 1.0/1.1 translation)
Exit criterion: the target test game's 3D/2D rendering appears on screen,
even if visually imperfect.

**Every task below is done, but the phase's literal exit criterion
(Double Dragon's own rendering on screen) is intentionally not yet
attempted — that's Phase 8's job** ("iteratively debug against the real
game, filling HLE API gaps as they're hit"), same relationship Phase
3/4's HLE work had to M1. What's actually proven here: the full `IGL`/
`IEGL` HLE surface needed for basic GLES1.x rendering exists, is real
vtable-order-verified, is forwarded to a real host OpenGL context, and
has been validated end-to-end against real compiled ARM code (our own
clean-room fixture, not Qualcomm's) producing a real, correctly-rendered,
color-interpolated triangle — not just unit tests driving the dispatch
logic directly. Known gaps deliberately left for when a real game
actually needs them: texture-combiner/lighting state, `glDrawElements`
real host-side index buffers (currently de-indexed, see below), and
compressed textures. (The CPU's multiply instruction, flagged here as
the more likely immediate blocker, was closed out in Phase 1 after
Phase 6 — see that phase's writeup.)

- [x] **Research: how BREW-era GLES actually reaches the OS — real
      architecture found, materially simpler than originally assumed.**
      Real Qualcomm sample `.mak` build rules show `EGL_1x.c`/`GLES_1x.c`/
      `GLES_ext.c` (BREW SDK-provided wrapper source) get statically
      compiled into *every game's own `.mod`* — GLES is not an OS service
      reached via `IShell`. Those wrappers dispatch `gl*`/`egl*` calls
      through two real AEE interfaces, **`IGL`** and **`IEGL`**, reached via
      global pointers (`gpIGL`/`gpIEGL`) set up once at startup. Found and
      read the real `AEEGL.h` (extracted from a genuine Qualcomm "OpenGL ES
      Extension for BREW SDK 4.x" installer already present in
      `research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/` —
      MSI → CAB → source, same clean-room method as every prior interface:
      read the real header for the ABI shape only, never copy/commit it).
      **Verified vtable slot order**: `AddRef, Release, QueryInterface`
      (confirmed via `INHERIT_IQueryInterface`'s companion access-macro
      block, same file, same order — same cross-check method used for
      every other interface), then 77 `gl*` methods for `IGL` (in
      declaration order, `glActiveTexture` → `glViewport`; 80 slots
      total), then 25 `egl*` methods for `IEGL` (`eglGetError` →
      `eglCopyBuffers`; 28 slots total) — counted programmatically
      against the real header, not eyeballed.
      Confirmed this applies to the actual target game too: Double
      Dragon's real `ddragonz.mod` contains the strings
      `eglGetColorBufferQUALCOMM` and `OpenGL.cpp`, consistent with the
      same statically-linked-wrapper pattern.
      **Consequence**: no need to design/ship our own GLES1.1
      fixed-function state machine (contra the original plan below) — we
      build `IGL`/`IEGL` HLE objects the same way as `IShell`/`IDisplay`/
      `IFile` (real vtable order, CPU call-out traps) and forward each
      implemented slot to a real host OpenGL context; the host GL driver
      does the actual fixed-function math. New wrinkle vs. prior
      interfaces: pointer args (`glVertexPointer`, `glTexImage2D`, index
      buffers) are emulated ARM addresses and must be copied into real
      host buffers at draw/upload time, not forwarded directly.
- [x] Build the full `IGL`/`IEGL` HLE vtable scaffold (all 108 slots — 80
      for `IGL`, 28 for `IEGL` — present at their real offsets,
      stub-by-default — same pattern as `IShell`). `core/brew/gl_hle.{h,cpp}`.
      One correctness wrinkle specific to this interface, not present in
      any prior one: real `IGL`/`IEGL` vtable slots do **not** receive the
      interface object pointer as an implicit first argument (confirmed
      from the real access macros, e.g. `#define IGL_glClear(p,a)
      AEEGETPVTBL(p,IGL)->glClear(a)` — only `a` is forwarded) — so R0
      holds the first *declared* argument for every `gl*`/`egl*` slot,
      unlike `IShell`/`IDisplay`/`IFile` where R0 is always `po`. Only the
      inherited `AddRef`/`Release`/`QueryInterface` slots keep the usual
      po-in-R0 convention.
- [x] Implement the EGL lifecycle slice (`eglGetError`, `eglGetDisplay`,
      `eglInitialize`, `eglTerminate`, `eglChooseConfig`,
      `eglCreateWindowSurface`, `eglDestroySurface`, `eglCreateContext`,
      `eglDestroyContext`, `eglMakeCurrent`, `eglSwapBuffers`) — matches
      the exact call sequence real sample app source uses
      (`simple_drawtexture.c`'s `InitGLSurface`/`FreeGLSurface`).
      `EGLDisplay`/`EGLSurface`/`EGLContext`/`EGLConfig` are simulated as
      small sentinel handles (`GlHle` never talks to a real host EGL
      implementation); `eglMakeCurrent` triggers exactly one real
      `GlBackend::CreateContext()` call, verified by a dedicated test
      (`EglMakeCurrentOnlyCreatesHostContextOnce`).
- [x] Implement a first core-GL slice (`glClear`, `glClearColorx`,
      `glViewport`, `glEnable`/`glDisable`, `glMatrixMode`,
      `glLoadIdentity`, `glOrthox`, `glFrustumx`, `glTranslatex`/
      `glRotatex`/`glScalex`, `glColor4x`) with `GLfixed`->float
      marshaling (16.16 fixed point, verified against hand-computed
      fixed-point values in tests, not just round-tripped), forwarded
      through a `GlBackend` seam (`core/brew/gl_backend.h`) — real host GL
      in the frontend (not wired up yet, see below), a recording
      `FakeGlBackend` in tests, same seam pattern as `Backend`.
      13 new tests (`tests/gl_hle_test.cpp`), including one that walks the
      full real EGL call sequence end to end and one that asserts every
      one of the 108 real vtable slots is present and non-null (not just
      the dozen with real behavior).
- [x] Implement vertex-array/draw-call marshaling (`glVertexPointer`/
      `glColorPointer`/`glTexCoordPointer`/`glNormalPointer`,
      `glEnableClientState`/`glDisableClientState`, `glDrawArrays`/
      `glDrawElements`) — the emulated-memory → host-buffer copy step.
      Array-pointer calls just record (pointer, type, size, stride) —
      emulated memory is only actually walked at draw-call time, once per
      enabled array, converting every component to a host float per the
      real GLES1.x per-type semantics (`GL_BYTE`/`GL_SHORT` cast directly,
      `GL_FIXED` via the existing 16.16 conversion, `GL_FLOAT`
      bit-reinterpreted, `GL_UNSIGNED_BYTE` additionally normalized by
      /255 for color arrays only, matching real `glColorPointer`
      semantics). `glDrawElements` shares the exact same extraction code
      as `glDrawArrays`, just keyed by an explicit index list decoded
      from emulated memory per its `GL_UNSIGNED_BYTE`/`GL_UNSIGNED_SHORT`
      type — output is equivalent, not indexed rendering on the host side
      (a documented simplification, not an oversight). `GlBackend` gained
      one new method, `DrawArrays(mode, GlVertexArrays)`, taking an
      already-fully-host-native struct — no `GLfixed` or emulated pointers
      ever reach a `GlBackend` implementation.
      6 new tests (`tests/gl_hle_test.cpp`): mixed `GL_BYTE`/
      `GL_UNSIGNED_BYTE` vertex+color gather with correct /255
      normalization, non-zero stride handling, `glDrawElements` with both
      `GL_UNSIGNED_BYTE` and `GL_UNSIGNED_SHORT` index types (including a
      repeated index, proving real gather-not-range semantics), the
      normal array's fixed 3-component shape (it has no `size` argument
      in the real API), and `glDisableClientState` actually excluding an
      array from the next draw. All 90 project tests green.
- [x] Implement texture upload (`glGenTextures`/`glDeleteTextures`/
      `glBindTexture`/`glTexParameterx`/`glTexImage2D`) with the same
      memory-copy approach. `glGenTextures`/`glDeleteTextures` round-trip
      `GLuint` names through emulated memory but let `GlBackend` own the
      actual namespace (`GenTextures(n, GLuint*)` fills a host-owned
      array, which `GlHle` then writes back into the app's buffer) — no
      separate ID-remapping table needed. `glTexImage2D` computes the
      real upload size from `(format, type, width, height)` — handling
      both the common `GL_UNSIGNED_BYTE` case (1 byte/component) and the
      packed 16-bit types (`GL_UNSIGNED_SHORT_5_6_5`/`_4_4_4_4`/`_5_5_5_1`,
      always exactly 2 bytes/pixel regardless of format) — then copies
      exactly that many bytes out of emulated memory into a host buffer
      before handing `GlBackend::TexImage2D` an already-resolved
      `GlTextureImage`; a null `pixels` argument (the real, legal "reserve
      storage now, upload later via glTexSubImage2D" call shape) is
      preserved as null rather than treated as an error.
      One correctness detail caught while reading the real spec, not
      assumed: `glTexParameterx`'s `param` is **not** GLfixed-converted
      even though it arrives through the same "x"-suffixed fixed-point
      slot as `glTranslatex`/`glColor4x` — every standard GLES1.x
      `glTexParameterx` `pname` (`MIN`/`MAG_FILTER`, `WRAP_S`/`WRAP_T`) is
      enum-valued, and the real spec's convention for enum-valued
      parameters passed through an "x" function is to forward the raw
      enum integer unconverted, not scale it by 65536 — verified with a
      dedicated test asserting `GL_LINEAR` (`0x2601`) arrives at
      `GlBackend` as exactly `0x2601`, not `0x2601/65536`.
      7 new tests (`tests/gl_hle_test.cpp`): ID round-tripping for
      `glGenTextures`/`glDeleteTextures`, raw passthrough for
      `glBindTexture`/`glTexParameterx`, `GL_RGBA`+`GL_UNSIGNED_BYTE`
      pixel-byte-exact upload, the null-pixels reserve-storage path, and
      packed `GL_UNSIGNED_SHORT_5_6_5` sizing (2 bytes/pixel regardless of
      `GL_RGB`'s 3-component format). All 97 project tests green.
- [x] Wire a real host OpenGL context into the SDL2 standalone frontend
      (context creation, `eglSwapBuffers` → actual buffer swap).
      `frontends/standalone/sdl2_gl_backend.{h,cpp}` (`Sdl2GlBackend`) is a
      thin, direct forward of every `GlBackend` method to the platform's
      real OpenGL 1.1 fixed-function entry points — no emulation-specific
      logic in this file at all, since `GlHle` already did every
      `GLfixed`/emulated-memory marshaling step before a call reaches
      here. Uses `find_package(OpenGL REQUIRED)` (`OpenGL::GL`), the
      standard cross-platform CMake mechanism — ships with the toolchain
      on Windows/macOS, needed `libgl1-mesa-dev`/`libglu1-mesa-dev` as a
      one-time local dev-machine install on Linux (same category of
      build-time-only dependency as Phase 3's X11 headers; I don't have
      sudo in this environment, so the user ran the install).
      **Verified with a real, screenshot-confirmed second window**, not
      just "it compiled": `main.cpp` opens a second `SDL_WINDOW_OPENGL`
      window alongside the existing `hello_brew` one, builds `IGL`/`IEGL`
      HLE objects backed by `Sdl2GlBackend`, and drives the exact real EGL
      call sequence (`eglGetDisplay` → `eglInitialize` →
      `eglChooseConfig` → `eglCreateWindowSurface` → `eglCreateContext` →
      `eglMakeCurrent` → `glClearColorx`(teal) → `glClear` →
      `eglSwapBuffers`) through `HleRuntime::CallArmFunction` — the same
      real vtable-dispatch path compiled ARM code would use, not a
      shortcut. Ran the built standalone frontend, used `gnome-screenshot`
      to independently capture both windows: the new GL window shows a
      real, solid teal fill (the exact color driven through the vtable
      chain, confirming real host OpenGL rendering actually happened end
      to end), and the original `hello_brew` window still shows its
      correct black-canvas-plus-white-block output, unaffected by the new
      wiring. One bug caught and fixed while wiring this up, before ever
      running it: the `eglChooseConfig` call's `num_config` output pointer
      is a stack-passed (5th) argument, and the first version of this code
      never set `SP` before calling it — would have read/written through
      whatever garbage address happened to be left over in memory.
- [x] **Validate against a real compiled GLES-exercising app — done via a
      clean-room fixture, not Qualcomm's actual sample source (same
      policy as `hello_brew.c`).** Real Qualcomm OGLES sample source
      (`simple_drawtexture.c` etc.) needs the real `EGL_1x.c`/`GLES_1x.c`
      wrapper compiled alongside it, which is real Qualcomm source we
      keep research-only/uncommitted — and more importantly, real samples
      pull in floating-point math and other complexity likely to hit the
      still-unimplemented CPU multiply instruction immediately. Instead,
      wrote `tests/fixtures/hello_gl/hello_gl.c`: our own minimal app,
      structurally identical to `hello_brew.c`'s `AEEMod_Load` ->
      `CreateInstance` -> `HandleEvent` lifecycle, but dispatching through
      the real, verified `IGL`/`IEGL` vtable slot order directly (no
      floating point anywhere — all fixed-point values are compile-time
      integer constants — so nothing here depends on soft-float library
      routines the interpreter doesn't support). It drives the full real
      EGL lifecycle, sets up an orthographic projection, and draws a
      single hardcoded red/green/blue triangle via
      `glVertexPointer`/`glColorPointer`/`glDrawArrays`. Compiled with
      the same `arm-none-eabi-gcc` toolchain as `hello_brew.c`; objdump
      confirms every vtable call compiles to a real `blx r3`/`blx r4`
      (register-form BLX, already implemented) with no `mul`/`mla`
      anywhere in the function.
      **New integration test** (`tests/gl_lifecycle_test.cpp`,
      `GlLifecycle.HelloGlAppDrawsRealTriangleThroughRealVtableDispatch`):
      loads the real compiled `.bin`, drives it through the real
      lifecycle exactly like `brew_lifecycle_test.cpp` does, and asserts
      on every stage a `RecordingGlBackend` observed — matrix
      mode/ortho/viewport values, the clear color/mask, and the exact
      triangle vertex positions and normalized colors gathered by real
      compiled ARM code from real emulated memory. Passed on the first
      run. 98/98 project tests green.
      **Also wired into the standalone frontend and screenshot-verified
      with real host OpenGL** (not just the recording fake): the second
      window now loads and runs `hello_gl.bin` for real instead of this
      file hand-driving the EGL/GL calls, and rasterizes a real,
      correctly color-interpolated RGB triangle via `Sdl2GlBackend`.
      **Found and fixed a genuine, previously-latent bug in the process,
      not just in the new code**: `hello_gl.bin` (and, confirmed via the
      same objdump check, `hello_brew.bin` too, unnoticed until now)
      isn't actually position-independent the way Phase 2 established
      `.mod`s should be — `arm-none-eabi-gcc` without `-fPIC`/ROPI flags
      bakes `&g_module`'s *absolute* link-time address into a literal
      pool (`ldr r2, [pc, #20]` loading a fixed `.word`) rather than
      computing it PC-relative. This was invisible before because every
      existing fixture/test always happened to load its `.mod` at
      exactly the address it was linked for (`0x00100000`) — this was
      the first thing in the project to attempt loading a second,
      independent `.mod` (at a different address, in the same memory
      space as `hello_brew`), which surfaced it immediately as a `0x0`
      function pointer three calls deep. A real BREW-compiled `.mod`
      (RVCT `armcc --apcs /ropi`) is genuinely position-independent and
      wouldn't have this problem — it's specific to our own gcc-built
      test-fixture convention, not a real ABI gap — so the fix was to
      give the GL demo its own independent `ArmInterpreter`/`HleRuntime`
      (a completely separate address space) rather than trying to make
      the fixture build truly position-independent, which is not
      currently needed anywhere else. Worth remembering if a future
      fixture ever needs genuine load-address independence.

## Phase 6 — Audio
Exit criterion: target test game's audio plays back correctly.

**Both codecs the real target game actually needs (PCM, MIDI) are done
and verified against real Double Dragon assets; IMA-ADPCM/MP3 are
deliberately deferred (see research below) — same relationship Phase
5's graphics work had to the actual game's rendering.** Not yet done:
driving any of this from real compiled ARM code (everything so far is
verified via direct HLE calls and unit tests, mirroring Phase 5's early
increments before `hello_gl.bin` existed) — a natural next step,
not attempted yet.

- [x] **Research: real `IMedia` interface + real target-game codec need —
      see ARCHITECTURE.md §3.6 for full detail.** Found and read the real
      `AEEMedia.h`/`AEEIMedia.h` (extracted from the same genuine BREW MP
      SDK installer used in Phase 4/5 — this time via a full NSIS
      extraction rather than a hand-picked file list, since the earlier
      partial extraction hadn't pulled the media headers). Confirmed a
      small 14-slot vtable, mostly a generic `SetMediaParm`/
      `GetMediaParm` command interface rather than one slot per feature.
      Also inspected Double Dragon's real `sound.ggz` directly (via
      `zeebulator_ggz_inspector`, already built and real-file-verified in
      Phase 2): 62 `.wav` (effects/voice) + 12 `.mid` (music), zero MP3.
      Checked a real extracted `.wav`'s `fmt ` chunk: plain 16-bit PCM,
      not IMA-ADPCM, on every file sampled. This directly reprioritizes
      the rest of this phase — PCM and MIDI are what M1 actually needs;
      ADPCM/MP3 are real BREW codecs (confirmed via the SDK's own
      `ctsoundmgr.c` sample, which genuinely uses ADPCM-named assets) but
      not required by the target title.
- [x] PCM decode/playback path (real target-game need, confirmed above —
      RIFF/WAVE container parsing; the audio data itself is already raw
      PCM, no real "decoding" math needed). `core/loader/wav.{h,cpp}`
      (`ParseWav`): handles 8-bit unsigned and 16-bit signed PCM,
      mono/stereo, chunk word-alignment padding (a real `bext` chunk in
      Double Dragon's own files exercises exactly this). Explicitly
      rejects non-PCM format tags (e.g. IMA-ADPCM) rather than
      mis-decoding them — same "reject, don't guess" philosophy as the
      CPU interpreter's `UnimplementedInstruction`. Cross-verified two
      ways: 7 synthetic-fixture unit tests (`tests/wav_test.cpp`), and a
      throwaway harness run against a real extracted Double Dragon `.wav`
      (13,411 real, correctly-shaped waveform samples at 22050Hz mono —
      not committed, matches every other real-file verification pass in
      this project).
- [x] Mixer + ring buffer feeding the Backend Abstraction Interface —
      `Backend::PushAudioSamples` has existed since Phase 0 and is
      finally used starting this phase. `core/audio/mixer.{h,cpp}`
      (`Mixer`): sample-accurate multi-voice mixing (mono duplicated to
      stereo, overlapping voices summed and clamped rather than wrapped
      on overflow, looping vs. one-shot voices, pause/resume preserving
      position), pushed to `Backend::PushAudioSamples` — which gained a
      `sample_rate` parameter as part of this work (previously
      undocumented and uncalled by anything). Deliberately does not
      resample — every real Double Dragon audio asset is uniformly
      22050Hz (confirmed in the research above), so a fixed-rate mixer is
      correct for the actual target game; a documented, revisit-if-needed
      simplification, not an oversight. 9 unit tests
      (`tests/mixer_test.cpp`).
- [x] Implement `IMedia` HLE (14-slot vtable, real order verified above)
      wired to the PCM path — `core/brew/media_hle.{h,cpp}`
      (`MediaHle`). `SetMediaParm(MM_PARM_MEDIA_DATA, ...)` assigns a
      virtual-filesystem-backed file (reusing Phase 4's GGZ-backed
      `VirtualFilesystem`) and decodes it immediately via `ParseWav`,
      matching real `IMedia`'s documented "SetMediaData puts IMedia in
      Ready state" behavior; `Play`/`Stop`/`Pause`/`Resume` drive a
      `Mixer` voice and update state accordingly; `GetState` also
      notices when a non-looping voice finished naturally since the last
      check. `MM_PARM_PLAY_REPEAT` maps repeat-forever (0) to the
      Mixer's boolean loop (exact repeat counts > 1 aren't tracked yet).
      `RegisterNotify` stores the callback but doesn't fire it yet —
      firing it on real transitions needs a periodic "tick" hook this
      project doesn't have wired into a real run loop yet (Phase 7/8
      territory, not a fundamental gap). Follows the same shared-vtable
      pattern as `FileHle` (many `IMedia` instances, one vtable).
      10 unit tests (`tests/media_hle_test.cpp`).
      **Wired into the standalone frontend with real SDL2 audio output,
      verified with unambiguous, concrete proof, not just "it compiled"**:
      split `Sdl2Backend` out to its own file
      (`frontends/standalone/sdl2_backend.{h,cpp}`) and gave it a real
      `SDL_AudioDevice` (`SDL_QueueAudio`-based); main.cpp builds a real
      `IMedia` object over a small self-synthesized 440Hz test tone
      (`tests/fixtures/test_tone.wav`, our own content, not a real game
      asset) and drives `SetMediaParm`/`Play` on it, with the event loop
      calling `Mixer::Mix` once per ~16ms tick. Needed
      `libasound2-dev`/`libpulse-dev` as a one-time local dev-machine
      install (same category of build-time-only dependency as Phase 3's
      X11 headers and Phase 5's OpenGL headers — SDL2 silently compiled
      out ALSA/PulseAudio/PipeWire/JACK support entirely despite the
      CMake cache saying "Wanted: ON" for all of them, because the actual
      dev headers weren't present at `FetchContent` build time; a real
      PulseAudio server was running the whole time, the build just
      couldn't link against it). After installing the headers and doing
      a full clean SDL2 rebuild: ran the standalone frontend and checked
      `pactl list sink-inputs` — found a real, live sink input
      (`application.process.binary = "zeebulator_standalone"`,
      matching PID, `Sample Specification: s16le 2ch 22050Hz`,
      `Corked: no`) — concrete, external, OS-level proof that real audio
      is actually flowing out through PulseAudio/PipeWire, the audio
      equivalent of Phase 3/5's screenshot verification. 124/124 project
      tests green throughout.
- [x] MIDI playback (real target-game need, confirmed above — genuine
      Standard MIDI Format 0 files, confirmed by parsing the real header
      bytes directly, not assumed; needed a real SMF parser + a simple
      synthesizer, not just a container parser like PCM).
      `core/loader/midi.{h,cpp}`: `ParseMidi` handles format 0/1 (format
      2 and SMPTE-timecode division are explicitly rejected, not
      mis-parsed), variable-length-quantity delta-times, running status
      (a real compression convention Standard MIDI Files use — repeated
      same-type events omit the status byte), meta events (only Set
      Tempo and End-of-Track are acted on; others are skipped by length),
      and merges note-on/note-off pairs across all tracks into one
      absolute-tick timeline, auto-closing any note never explicitly
      turned off at the track's end. `RenderMidiToPcm` converts that into
      PCM: a simple sine-wave synthesizer, one tone per note scaled by
      velocity with a short linear fade in/out to avoid clicks between
      notes, tick positions converted to real seconds via the file's
      full tempo-change map (not just tempo at tick 0 — a real Double
      Dragon track changes tempo repeatedly mid-file, confirmed below).
      **Deliberately no instrument/timbre modeling** (every note sounds
      the same sine tone regardless of MIDI program-change events, and
      the percussion channel isn't treated specially) — a documented,
      honest scope for a first correct-but-crude synthesizer, not a full
      General MIDI implementation; revisit if the target game's music
      turns out to need real instrument timbres to be recognizable.
      Both codecs converge on the same `WavAudio` shape, so `MediaHle`
      dispatches purely by file extension (`.wav` → `ParseWav`, `.mid`/
      `.midi` → `ParseMidi` + `RenderMidiToPcm`) and everything
      downstream (Mixer, playback state, `IMedia` HLE) is fully
      codec-agnostic.
      **Cross-verified against all three real Double Dragon `.mid` files
      checked**, not just synthetic fixtures: `bgm_1_0.mid` → 1788 notes,
      163 BPM, 47.5s; `bgm_2.mid` → 1811 notes, a real gradual tempo
      ramp (82→87.4 BPM across the first few tempo-change events), 109s;
      `bgm_9.mid` (the smallest file, 1034 bytes) → 110 notes, 3.96s —
      all durations and note counts are exactly what you'd expect from
      the respective file sizes, strong independent confirmation the
      tempo-map/tick-to-seconds conversion is correct on real data, not
      just hand-built test cases.
      11 new unit tests (`tests/midi_test.cpp`) covering VLQ/running-
      status parsing, chords, unclosed notes, explicit tempo changes, and
      a zero-crossing-count sanity check that a max-velocity A4 note
      actually renders audio near 440Hz. One real bug caught and fixed
      *in the test suite itself* while writing these (not the parser):
      an early version of the "unclosed note" test left a dangling
      delta-time VLQ with no attached event — invalid MIDI, since a
      delta-time must always be immediately followed by an event — which
      the parser (correctly, per real running-status semantics) then
      misinterpreted as a second bogus note; fixed by constructing that
      test's track by hand instead of relying on the shared helper's
      assumptions.
      Also wired a second live demo into the standalone frontend
      (`tests/fixtures/test_tune.mid`, our own tiny 4-note original
      arpeggio) alongside the WAV tone, verified the same way — a real,
      live `pactl` sink input from the running process. 10 new
      `MediaHle` coverage assertions total across both codecs
      (`tests/media_hle_test.cpp`). 136/136 project tests green.
- [ ] IMA-ADPCM decoder — deferred, not needed by the target title (see
      research above); revisit if a future title actually needs it
- [ ] MP3 decoder integration — deferred, not needed by the target title
      (see research above); MP3 patents have expired so there's no
      licensing blocker whenever it does become needed

## Phase 7 — Input
Exit criterion: target test game responds correctly to controller input.

- [ ] Implement `IHID` HLE (Zeebo Z-Pad extension) — ARCHITECTURE.md §3.7
- [ ] Standalone frontend: SDL2 gamepad/keyboard → `ZPadState` mapping
- [ ] Default input mapping matching a standard Xbox-layout controller

## Phase 8 — First Playable Commercial Game
Exit criterion: **M1 from PRD §7** — target title (Double Dragon) fully
playable start-to-finish at full speed, standalone build.

- [ ] Iteratively debug against the real game, filling HLE API gaps as they're hit
      **First real attempt, honestly reported**: built `tools/game_probe.cpp`,
      a dev tool that drives Double Dragon's real, compiled
      `mod/274754/ddragonz.mod` through the actual BREW app lifecycle —
      `AEEMod_Load` → `IModule::CreateInstance(ClsId=274754)` →
      `HandleEvent(EVT_APP_START)` — using the real `IShell`/`IDisplay`/
      `IGL`/`IEGL`/`IFile`/`IMedia` HLE built in earlier phases, real
      `data.ggz`/`sound.ggz` assets mounted via `VirtualFilesystem`, and a
      real SDL2 window/GL context/audio device (not a headless stub).
      **Result: does not yet load.** The game does not successfully reach
      `EVT_APP_START`. Two real, previously-unknown facts about the real
      BREW ABI were discovered and corrected along the way (via a genuine
      NSIS extraction of the Qualcomm BREW MP SDK installer, not guessed):
        - `EVT_APP_START = 0`, not `1` as our own `hello_brew.c`/
          `hello_gl.c` SDK fixtures had assumed (`platform/system/inc/AEEEvent.h`).
          Those fixtures are self-consistent, so this never broke anything
          internally — but it was wrong for real game code.
        - The real `AEEAppStart` struct is
          `{ int error; AEECLSID clsApp; IDisplay *pDisplay; AEERect rc; const char *pszArgs; }`
          — 5 fields, not 4 — and the real `AEERect` is
          `{ int16 x, y, dx, dy; }` (8 bytes), not `{ int x, y, dx, dy; }`
          (16 bytes) as our own fixtures assumed. Correct total size: 24 bytes.
      **The actual blocker**: disassembling the real `.mod` directly
      (`arm-none-eabi-objdump -D -b binary -m arm`, the authoritative way to
      read a raw flat binary — manual hex decoding of this same function
      produced real mistakes before switching to the real tool) shows
      `AEEMod_Load`'s body computing `r0 = <module's own loaded base
      address>` via PC-relative addressing, then executing
      `ldr r0, [r0, #-4]` — reading a pointer from 4 bytes *before* the
      module's own base. This is the real ARM RVCT "ROPI" (Read-Only
      Position-Independent) convention: compiled code expects the real BREW
      OS loader to place a pointer to a runtime-allocated global-data/
      static-base segment immediately before the module image in memory.
      Zeebulator's `.mod` loader (`core/loader/mod.{h,cpp}`) does not
      currently provide this — nothing is written at `moduleBase - 4` — so
      that load reads whatever was already in emulated memory there (zero,
      since it was never written), and the module jumps through a garbage
      function pointer built from it.
      **Why this wasn't obvious at first**: the ARM interpreter's existing,
      correct convention of decoding never-written (zero-filled) memory as
      harmless `ANDEQ r0,r0,r0` no-ops meant the very first attempt produced
      a *plausible-looking but meaningless* "success" — 262,237 steps of
      no-ops walking PC from address 0 back up to the module's own base
      address, silently re-entering `AEEMod_Load`, and returning
      `R0=0x00000001` (a bogus "module pointer"). This was **not** a CPU
      interpreter bug (it behaves exactly as specified for the bytes it's
      given) — it's the loader's missing static-base setup producing a
      false positive that looked like progress. Caught by building
      `CallArmFunctionChecked`, a diagnostic wrapper that tracks whether PC
      ever leaves the loaded module's address range and reports it as a
      loud warning rather than silently accepting whatever `R0` comes back.
      With that check in place, the real, honest result is: PC leaves the
      module's range after only 37 steps (reading the garbage pointer and
      jumping through it), and the tool now stops and reports this cleanly
      instead of reporting a false "AEEMod_Load OK".
      **Net assessment**: real compiled Double Dragon code now runs
      further and more meaningfully than at any prior phase — past the
      entry-stub validation exercised by Phase 2's `mod_probe` (which never
      called real `AEEMod_Load` with real arguments) and into real
      argument-validated logic inside the actual function. It still does
      not load. The next concrete step (not yet attempted) is teaching
      `core/loader/mod.{h,cpp}` to allocate and populate a static-base
      segment at `moduleBase - 4`, per the real ROPI convention — this is a
      loader/runtime-support gap, not an HLE API gap, so it's tracked here
      rather than under a specific interface.
      **Static-base fix implemented**: `core/brew/mod_runtime.{h,cpp}`
      (`ModRuntime`). Confirmed the exact table layout needed by reading
      the real bundled SDK reference source `AEEModGen.c` (found under
      `research/docs/sdk_installer_extract/`): `AEEMod_Load` calls the
      standard helper `AEEStaticMod_New(sizeof(AEEMod), pIShell, ph,
      ppMod, NULL, NULL)`, which does
      `MALLOC(nSize + sizeof(IModuleVtbl))` — and `IModuleVtbl` is exactly
      4 function pointers (`AddRef`/`Release`/`CreateInstance`/
      `FreeResources`, 16 bytes), matching the disassembly's `add r0, r5,
      #16` immediately before the indirect call through table offset
      `0x68`. Cross-checked the PC-relative literal at file offset
      `0x220c` in the real `ddragonz.mod` directly (`r0 = pc(0x2188) +
      0xffffde78 = 0x00000000`), confirming the computed address is
      exactly the module's own load base — i.e. the table pointer really
      does live at `moduleBase - 4` for any load address, as the ROPI
      convention requires. `ModRuntime::Install()` writes a table pointer
      there and populates only that one confirmed slot (offset `0x68`)
      with an HLE trap implementing a simple bump-allocator `MALLOC`; all
      other table offsets remain intentionally unmapped (unconfirmed).
      5 new tests (`tests/mod_runtime_test.cpp`): table pointer placement,
      a real allocation call through the HLE trap, non-overlapping
      successive allocations, 4-byte alignment, and out-of-heap-space
      returning NULL.
      **Verified against the real game**: reran `zeebulator_game_probe`
      against the real `ddragonz.mod`/`data.ggz`/`sound.ggz`.
      `AEEMod_Load` now succeeds *legitimately* — no
      wandered-outside-module warning, a real module pointer
      (`0x80300000`, the heap's first real allocation) rather than the
      old false-positive `0x00000001`. Execution now reaches
      `IModule::CreateInstance(ClsId=274754)` and runs real, in-bounds
      module code to completion, but that call itself returns a genuine
      `EFAILED` (1) without writing an object pointer — a new, different,
      and deeper real gap than the loader issue this fix closes.
      **Continued the investigation**: disassembled `CreateInstance`'s
      real body (`arm-none-eabi-objdump` again, not manual decoding).
      With the *wrong* guessed `ClsId` (`274754`, the `.mod` file's
      containing directory name — just a filesystem/distribution
      convention, it turns out, not the app's real class ID), the real
      code path legitimately hit a `cmp ClsId, #0x0102f789; bne EFAILED`
      check and correctly reported failure — a real, trustworthy
      rejection, not a bug. Cross-checked the literal directly against
      the raw file bytes at offset `0x738` (`0x0102f789`, exactly the
      value the disassembly compares against) to be certain. **Rerunning
      with the real ClsId (`0x0102F789` / `16971657`) gets past that
      check** and reaches real, deeper application logic: `AEEClsCreateInstance`
      tail-calls the real, standard SDK helper `AEEApplet_New` (identified
      by matching disassembly line-by-line against the real reference
      source `AEEAppGen.c`, also found under
      `research/docs/sdk_installer_extract/`), which mallocs a real
      `AEEApplet` struct (via the same confirmed MALLOC slot) and then
      calls `ISHELL_CreateInstance(pIShell, AEECLSID_DISPLAY, &m_pIDisplay)`
      — real compiled app code obtains its `IDisplay` through `IShell`,
      not directly via `EVT_APP_START` as our own `hello_brew`/`hello_gl`
      SDK fixtures do. `AEECLSID_DISPLAY`'s real value (`0x01001001`) was
      read directly out of the disassembly (the literal loaded right
      before the `ISHELL_CreateInstance` call), not guessed.
      Our `IShell::CreateInstance` was a blind stub returning success
      without writing `*ppObj` — meaning `m_pIDisplay` stayed `NULL`,
      which `AEEApplet_New` correctly treats as fatal (matching the real
      reference source) and tries to clean up via `FREE(pme)` — landing
      on a **second** unmapped static-base table slot, offset `0x6c`
      (`FREE`, immediately after `MALLOC` at `0x68`), causing the same
      jump-through-null-pointer wander as before.
      **Fixed both**: `core/brew/ishell.{h,cpp}` — `BuildIShell` became
      the stateful `IShellHle` class with `RegisterInstance(clsId, ptr)`,
      so callers can register real singleton objects (our already-built
      `IDisplayHle` instance) for `IShell::CreateInstance` to hand back;
      an unregistered ClsId now returns real `EFAILED` instead of a lying
      "success" with an unwritten pointer. `core/brew/mod_runtime.{h,cpp}`
      gained the `FREE` slot at offset `0x6c` (a no-op trap — consistent
      with the bump allocator's documented no-free-list limitation).
      3 new `IShellHle` tests in `tests/brew_test.cpp`.
      **Reran against the real game**: with all three fixes, `CreateInstance`
      now gets substantially further — `*ppObj` is written
      (`0x80300024`, a real, live `AEEApplet` pointer) and it returns real
      `SUCCESS` (0) — but `tools/game_probe.cpp`'s wandered-outside-module
      check still (correctly) flags the result as untrustworthy: at real
      step 168, different real code elsewhere in the module reads *yet
      another* static-base table offset (`0xc0`), unrelated to
      `MALLOC`/`FREE`, and jumps through it while still zero/unmapped.
      This is the same static-base table, but evidently used as a much
      larger, general-purpose runtime-services jump table (real BREW's
      `AEEStdLib` helper dispatch is a plausible candidate, based on the
      calling pattern seen at this new site) — mapping it out fully looks
      like it needs its own systematic pass (identifying each offset's
      real function one at a time against the reference SDK source, the
      same way `MALLOC`/`FREE` were confirmed), not a single quick fix.
      **Scanned the whole binary instead of tracing one call site at a
      time**: wrote a one-off script (`arm-none-eabi-objdump` output fed
      through a small Python pattern-match for "read static base, then
      index the result") to find every distinct offset `ddragonz.mod`
      actually dereferences off this table. Real, bounded result — not
      "dozens", nine distinct offsets total: `0x4`, `0x8`, `0x14`, `0x68`
      (`MALLOC`), `0x6c` (`FREE`), `0xb0`, `0xc0`, `0xe8`, `0x13c`. Offset
      `0xc0` dominates by far (138 of the real call sites). Checked three
      independent `0xc0` call sites and found byte-for-byte identical
      code shape at all three: call the slot with no argument, read
      offset `+12` from the result, dereference that once more to reach
      a vtable, and call vtable slot 2 (`CreateInstance`) on it — i.e.
      `ISHELL_CreateInstance(pIShell, ClsId, ppObj)` where `pIShell` comes
      from an ambient "current app context" instead of being passed in
      explicitly. This is the real mechanism behind `AEEStdLib` helper
      macros that don't take an explicit `IShell*` argument.
      **Implemented**: `core/brew/mod_runtime.{h,cpp}` gained
      `SetShellInstance()` and a `GetAppContext` trap at offset `0xc0`
      that returns a small context struct with the real `IShell` pointer
      at field `+12` (written fresh on every call, so call order with
      `SetShellInstance()` doesn't matter). `ModRuntime`'s constructor
      gained a `context_address` parameter for where that struct lives.
      4 new tests in `tests/mod_runtime_test.cpp` (FREE doesn't crash,
      `GetAppContext` returns the registered shell pointer at the
      confirmed offset, both call orderings of `SetShellInstance()`).
      **A second, unrelated bug found and fixed in `tools/game_probe.cpp`
      itself** (not an emulation gap): `*ppObj` from `IModule::CreateInstance`
      is the `IApplet*` object itself, not a callable `HandleEvent`
      pointer — real `HandleEvent` is vtable slot 2 of *that* object
      (`AddRef=0, Release=1, HandleEvent=2`, confirmed against the real
      `AEEAppGen.c` reference source's `IAppletVtbl` init order), the same
      double-indirection already used for `IModule::CreateInstance`
      itself just above it. The tool was also passing `0` instead of the
      real applet pointer as `HandleEvent`'s own `po` ("this") argument.
      Both fixed.
      **Full result — first complete, successful BREW app lifecycle for a
      real commercial game module**: reran `zeebulator_game_probe`
      against the real `ddragonz.mod`/`data.ggz`/`sound.ggz`.
      `AEEMod_Load` → `IModule::CreateInstance` →
      `IApplet::HandleEvent(EVT_APP_START)` all complete with **no**
      wandered-outside-module warning and **no** `UnimplementedInstruction`
      — `HandleEvent` returns real `TRUE` (1), meaning the real compiled
      app reports it successfully handled its own startup event. Captured
      a real screenshot of the live SDL window afterward (`xwd` +
      `ffmpeg`, same verification rigor as Phase 5): solid black, which is
      the *correct*, expected result here, not a failure — nothing has
      driven a draw call yet, since only the one `EVT_APP_START` event was
      ever delivered and no per-frame/timer event pump exists yet.
      **Honest scope**: this is real, verified initialization success —
      not yet "playable" (PRD/Phase 8's actual exit criterion). Real
      gameplay would need at minimum: a `SetTimer`/timer-tick mechanism
      (currently `IShell::SetTimer` is a blind stub), likely
      `EVT_APP_ACTIVATE` and other lifecycle events real BREW sends after
      `EVT_APP_START`, and probably several of the six still-unmapped
      static-base offsets found by the scan above (`0x4`, `0x8`, `0x14`,
      `0xb0`, `0xe8`, `0x13c`) as deeper game logic starts executing.
      **Implemented the real per-frame game loop**: traced
      `HandleEvent(EVT_APP_START)`'s one real HLE call and found it's
      `ISHELL_SetTimer(shell, 33, callback=0x1239dc, pUser=applet_ptr)` —
      the real game registering its own tick callback, exactly the
      standard "self-rearming `SetTimer`" BREW game-loop pattern
      confirmed against a real bundled SDK sample
      (`research/samples/conftest_source/conftest/conftest.c`'s
      `conftest_TimerNotify`, which re-arms itself via `ISHELL_SetTimer`
      as its own last action -- real BREW timers are one-shot, not
      repeating). `IShell::SetTimer`/`CancelTimer` (vtable slots 11/12)
      are now real: `IShellHle` tracks pending timers and exposes
      `Tick(elapsed_ms)`, which the frontend calls once per loop
      iteration and drives any newly-expired callback through
      `CallArmFunctionChecked`. 4 new `IShellHle` tests.
      **Mapped two more static-base slots this way, real disassembly at
      each step, not guesses**:
        - Offset `0xb0` (4 real call sites) is `GETUPTIMEMS`: called
          twice with no argument around a chunk of work, with
          `second_result - first_result` used immediately after as an
          elapsed-ms delta (`sub r1, r0, r7` in the real timer-callback
          disassembly) -- the standard elapsed-time-measurement idiom.
          `ModRuntime::Tick()` now advances a millisecond counter this
          slot returns.
        - Offset `0x4` (10 real call sites, the single most common after
          `0xc0`) is `MEMSET`: a real call site
          (`ddragonz.mod` file offset `0x1f9b0`) disassembles to exactly
          `memset(r4+46, 0, 10)` -- `r0`=dest, `r1`=fill value, `r2`=byte
          count, matching ANSI `memset` precisely. Real ROPI-compiled
          code apparently routes even plain C library calls through this
          table, not just AEE/BREW-specific ones -- worth remembering
          when mapping the remaining offsets (`0x8`, `0x14`, `0xe8`,
          `0x13c`; `0x8`'s one real call site disassembles to a very
          `strcpy`-shaped 2-argument call, `strcpy(dest, src)`, but
          wasn't fully confirmed/implemented this pass).
      2 new tests total for these (`ModRuntime.GetUpTimeMsStartsAtZeroAndAdvancesWithTick`,
      `ModRuntime.MemsetFillsExactlyTheRequestedRangeAndReturnsDest`).
      **Reran against the real game**: the timer callback now fires for
      real, every frame, and runs measurably deeper each time a slot gets
      mapped -- 14 real steps before `SetTimer` existed at all, 41 after
      `GETUPTIMEMS`, 153 after `MEMSET`. The real per-tick game logic
      currently stops on a **new, different** class of gap, traced down
      to a specific real function this time (`ddragonz.mod` offset
      `0x23a18`, called with `r0 = &applet[0x140]`, an *embedded*
      sub-object living inside the applet's own struct, not a separately
      `MALLOC`'d one): it copies a couple of fields from a template
      object cached at `applet[0x140]+12`, then reads `*(applet[0x140])`
      -- expecting the embedded sub-object's own vtable pointer to
      *already* be populated -- and calls vtable slot 18 (offset `0x48`)
      on it. That memory was never written by anything in the traced
      execution (no static-base call touches it, and our own `MALLOC`
      doesn't zero-fill), so it reads back 0 and the call jumps through
      NULL. This isn't a missing runtime-library slot -- it's most likely
      a real `str <compile-time-vtable-address>, [applet+0x140]`
      "placement new" that should have happened earlier, inside
      `HandleEvent(EVT_APP_START)`'s own real (non-HLE) logic, which
      wasn't traced at per-instruction granularity this session (only
      its one real HLE call was). Root-causing this means tracing
      `HandleEvent`'s full internal control flow, not just another
      library-call mapping -- a different, larger kind of investigation
      than the last several fixes.
      **Root-caused, and the actual cause was simpler than expected**:
      full per-instruction tracing of `HandleEvent(EVT_APP_START)` shows
      it does *nothing* for `evt==0` except call `ISHELL_SetTimer` and
      return TRUE (confirmed via real disassembly of the app's own event
      dispatcher at `ddragonz.mod` offset `0xc5e0`, which switches on the
      real BREW event codes with `evt==0` landing on exactly that path)
      -- so `applet+0x140` was never going to be set there. Tracing
      further up, into the *already-passing* `CreateInstance` call (with
      full tracing this time, not just its HLE calls), found the real
      write: `ddragonz.mod` offset `0x1b5e4` does
      `*(applet+0x140) = *(our_context_struct + 20)` -- reading a
      **second field** of the same "app context" struct our own
      `ModRuntime::GetAppContextImpl` (offset-`0xc0` slot) returns, one
      we'd only ever populated at offset `+12` (the `IShell` pointer).
      Offset `+20` was never written, so it read back 0 and the NULL
      propagated forward into `applet+0x140`, surfacing several function
      calls later as the crash actually being chased. Confirmed what
      offset `+20` should be by continuing to trace forward: the value
      written into `applet+0x140` later gets dereferenced and called on
      vtable slot 18, which -- checked directly against the real
      `AEEIDisplay.h`'s `INHERIT_IDisplay` macro -- is exactly
      `SetClipRect(po, pRect)`, called with `pRect=NULL`. So context
      offset `+20` is the current app's `IDisplay` pointer, sibling to
      `IShell` at `+12`. `ModRuntime` gained `SetDisplayInstance()`.
      **Bonus finding**: `IDisplayHle`'s vtable only had the first 13 of
      `IDisplay`'s real slots built (an incorrect assumption from Phase 3
      that that was the full pre-BREW-MP interface) -- extended it to
      all 26 real slots per `AEEIDisplay.h` (through `SetPrefs`), stubbed
      beyond the four with real behavior (`AddRef`/`Release`/`DrawText`/
      `Update`), so slot-18 and beyond no longer read past the array.
      **Also mapped a sixth static-base slot** the same session, offset
      `0x14`: a real call site (`ddragonz.mod` offset `0x23b00`) calls it
      with one string-pointer argument and immediately does
      `add r1, r0, #1` on the result -- the classic `strlen(s) + 1`
      buffer-sizing idiom, matching ANSI `strlen` exactly.
      6 new tests total across `mod_runtime_test.cpp`
      (`GetAppContextSlotReturnsDisplayInstanceAtConfirmedOffset`,
      `StrlenReturnsLengthExcludingNullTerminator`,
      `StrlenReturnsZeroForEmptyString`, plus the earlier increment's).
      **Reran against the real game**: the tick callback now runs
      measurably further with each fix -- 153 (before this round) → 229
      (after the `IDisplay` context field) → 238 (after `STRLEN`) real
      steps. The very next real call the trace already shows waiting
      (right after `STRLEN` returns, at `ddragonz.mod` offset `0x23b18`)
      is a **seventh** static-base slot, offset `0xe4` -- not caught by
      the earlier whole-binary scan (a gap in that scan's own pattern-
      matching window, worth revisiting) -- called with a string
      pointer, a stack buffer, and a size constant `0x200`, shaped like
      `strlcpy`/`strncpy`.
      **Implemented it**: unlike the other libc-shaped slots, this one's
      exact real name/signature wasn't matched against a reference
      source (the argument order -- `src, strlen(src)+1, dest, cap` --
      doesn't cleanly match a standard `strlcpy(dest, src, len)`), but
      the copy semantics are unambiguous from the calling convention
      alone: `n = min(strlen_plus_one, cap); memcpy(dest, src, n)`.
      Implemented exactly that (`ModRuntime::BoundedStrcpyImpl`). 2 new
      tests (copies up to the requested length; never writes past the
      cap).
      **Reran against the real game and got real visible output for the
      first time**: no wander, no `UnimplementedInstruction`, no
      "did not complete trustworthily" -- the tick callback ran cleanly
      across roughly 500 real frames over an 8-second observation window
      with no new gap surfacing. Captured a live screenshot (`xwd` +
      `ffmpeg`): a small white rectangle now renders in the top-left of
      the window (our `IDisplayHle::DrawText` placeholder-block
      behavior, so not real game graphics yet, but real proof the game
      is now genuinely calling into `IDisplay` draw methods every frame
      through the fully-resolved static-base table). This is the first
      point all session where something is visibly different on screen.
      **Decoded the full steady-state per-tick call sequence**: added a
      lightweight HLE-call-only trace mode (`hle_trace`, logs just the
      trap address + registers for real interface calls, not every ARM
      instruction) to `tools/game_probe.cpp` and captured 10 consecutive
      ticks. Every tick does the *identical* real sequence: `GETUPTIMEMS`
      → 4× `MEMSET` (clearing small sub-buffers) → `IDISPLAY_SetClipRect(NULL)`
      → `IDISPLAY_DrawRect(NULL, clrFrame=0xffffffff, clrFill=0xffffff00)`
      → `IDISPLAY_SetColor(1, 0x00000000)` → six repetitions of
      `[STRLEN → bounded-strcpy → IDISPLAY_DrawText]` (six distinct HUD/
      menu strings) → `ISHELL_SetTimer(..., 100, callback, applet)` to
      re-arm. `DrawRect` and `SetColor` were still stubs, so the
      background fill and text color were both silently no-ops.
      **Implemented both, for real**: `IDisplayHle` gained
      `DrawRect`/`SetColor`. Treats `RGBVAL` as the common real-BREW
      `0x00RRGGBB` packing (`MAKE_RGB(r,g,b)`, per the real
      `AEEIDisplay.h` reference doc comment) -- this specific bit layout
      wasn't independently confirmed against a real header this session,
      unlike the vtable slot order itself, which was; documented as an
      assumption in `idisplay.h`. `DrawRect` fills the given `AEERect`
      (or the whole screen if `pRect` is `NULL`) with `clrFill`, no
      border rendering yet. `SetColor` collapses all `AEEClrItem` slots
      into one "current color" `DrawText` now uses instead of a
      hardcoded white (a documented simplification). 4 new tests
      (`IDisplayHle.DrawRectWithNullRectFillsWholeScreen`,
      `DrawRectWithExplicitRectFillsOnlyThatArea`,
      `SetColorChangesDrawTextColorAndReturnsPrevious`, plus updated
      `DrawTextThenUpdatePushesCorrectFrame` coverage).
      **Reran against the real game**: the screenshot now shows a solid
      **yellow background with black text** -- not solid-color noise,
      a plausible, deliberate real UI color scheme, which is a strong
      positive signal the reverse-engineered `RGBVAL` packing and
      `AEERect` layout assumptions are correct. This is the first frame
      all session with real, meaningful visual content.
      **Confirmed stable over a longer window**: reran for 35 real
      seconds (~2000 real frames) with `hle_trace` capped to the first
      10 ticks for output volume -- no wander, no exception, no "did not
      complete trustworthily" for the entire run.
      **Wired up real keyboard input**: real disassembly of the app's
      own `HandleEvent` dispatcher (`ddragonz.mod` offset `0xc640`)
      shows real BREW event codes `evt==0x101`/`0x102` both calling a
      helper (offset `0x1a3c4`) that does
      `sub r0, wParam, #0xe021; cmp r0, #22; addls pc, pc, r0, lsl #2` --
      a jump table converting `wParam` values in `[0xe021, 0xe021+22]`
      into per-key bitmask flags (OR'd in for `0x101`, AND-cleared for
      `0x102`) -- i.e. real key-down/key-up events, confirmed from the
      binary itself even though the exact real `AVK_*` numeric mapping
      wasn't independently verified against a header this session.
      `tools/game_probe.cpp` now forwards real SDL key events into
      `HandleEvent(applet, 0x101/0x102, wParam, 0)` -- number keys 0-9
      map to the confirmed range's first 10 offsets, arrow keys to the
      next four (a real, disassembly-grounded event *mechanism*, but an
      exploratory, unconfirmed *specific key* mapping, documented as
      such in code).
      **Tested with real synthetic key events** (`python-xlib`'s XTest
      extension, since the harness has no physical keyboard): number
      key `1` and the up/down/left arrows all dispatch cleanly through
      real app code and return `TRUE` (handled), matching the confirmed
      bitmask-flag mechanism -- no visible change on screen yet since
      `DrawText` still only draws placeholder blocks rather than real
      glyphs, so distinguishing key-driven state changes visually isn't
      possible yet either. The right-arrow-mapped code hit a **new**
      gap (same "wandered into scratch memory, hit an invalid
      instruction" shape as every other gap this session) -- caught
      cleanly by `game_probe`'s own exception handling (prints and
      continues) rather than crashing the tool, confirming that
      safety net holds up under real, unplanned input too. Not yet
      root-caused.
      **Implemented real glyph rendering**: `core/brew/font5x7.{h,cpp}`,
      a small, self-authored 5x7 bitmap font (uppercase Latin letters,
      digits, space; anything else falls back to a small box) --
      hand-designed for this project, not extracted from any real game
      or copied from a third-party font table, consistent with
      `CONTRIBUTING.md`'s clean-room policy. `IDisplayHle::DrawText` now
      rasterizes real per-character glyphs (folding lowercase to
      uppercase, since the font only has one case) instead of a solid
      placeholder block. Updated the two existing `DrawText` tests
      (`IDisplayHle.DrawTextThenUpdatePushesCorrectFrame`,
      `BrewLifecycle.HelloBrewAppDrawsTextAndUpdatesScreen`) to check
      real glyph-shaped pixel patterns instead of solid-block bounds,
      and added 5 new `Font5x7` unit tests.
      **Reran against the real game**: the screenshot now shows
      genuinely distinct per-character shapes for the first time (not a
      solid block) -- real, verifiable progress -- but most of the
      drawn characters fall back to the generic box rather than a real
      letter, meaning the six real HUD strings likely use lowercase
      and/or punctuation the current font doesn't cover yet. Expanding
      the font's coverage (lowercase, common punctuation) would likely
      make the real text legible; not done this pass.
      **Root-caused the key-input gap**: traced the right-arrow-mapped
      code path with full instruction tracing and found **two** more
      real static-base slots, both hit only by that specific jump-table
      branch (not the up/down/left/digit paths already tested clean):
        - Offset `0x0` (the table's very first word, never read by
          anything up to this point) is **MEMCPY** -- a real call site
          (`ddragonz.mod` offset `0x223b4`) reads it with a bare
          `ldr r3, [r0]` (no displacement) and calls `(dest, src, n=36)`,
          sitting naturally right before `MEMSET` at offset `0x4`.
        - Offset `0x8` is **STRCPY** -- the same offset flagged as
          "strcpy-shaped but unconfirmed" several rounds ago. A real
          call site (`ddragonz.mod` offset `0x1a3e0` onward) passes just
          `(dest, src)`, no length, matching unbounded
          `char *strcpy(char*, const char*)` exactly.
      Both implemented in `ModRuntime` (`MemcpyImpl`/`StrcpyImpl`) with
      2 new tests each behavior confirms (exact-range copy, stops at the
      null terminator, returns `dest`).
      **Reran against the real game**: sent all 14 mapped keys (4 arrows
      + digits 0-9, 28 key-down/key-up events total) -- every single one
      now dispatches cleanly through real app code with zero exceptions
      and zero wandering. The key-input path is fully clean for the
      first time. Static-base table slots confirmed so far: `0x0`
      (MEMCPY), `0x4` (MEMSET), `0x8` (STRCPY), `0x14` (STRLEN), `0x68`
      (MALLOC), `0x6c` (FREE), `0xb0` (GETUPTIMEMS), `0xc0`
      (GETAPPCONTEXT), `0xe4` (bounded copy) -- nine confirmed slots
      total.
      **Solved the mystery of what the six on-screen strings actually
      say, and why**: added a temporary debug print of `DrawText`'s raw
      code units (removed after use, not committed) and found they
      aren't `AECHAR`/UTF-16 at all -- they're a plain 8-bit `char*`
      being read 16 bits at a time, so each "code unit" is really two
      swapped ASCII bytes. Decoded, the six strings are fragments of one
      real message: **"Memory is insufficient. Please start \[the game\]
      after finishing \[the\] other application \[...\] by pushing the
      button."** -- a genuine BREW low-memory warning dialog, not
      gameplay HUD text, which is why it renders identically every
      single frame.
      **Root-caused *why* the game thinks memory is insufficient** (a
      real, hardware-realistic diagnosis, not a guess): traced the
      renderer back through a generic "draw N strings from a table"
      helper (`ddragonz.mod` offset `0x49f0`) to a per-state dispatch
      keyed by a field at `applet+0x24`, confirmed live via a temporary
      watchpoint on that exact address (removed after use) to show it's
      written twice during `CreateInstance` -- to `0` (proceeding
      normally), then immediately to `1` (the "show warning" state) --
      *before* `HandleEvent(EVT_APP_START)` even runs. Two candidate
      "if this fails, set state=1" checks earlier in the same function
      were ruled out by disassembly (both real called functions
      unconditionally return success in this binary). The real cause:
      `ISHELL_CreateInstance(shell, ClsId=0x01002001, &field)` -- a real
      class ID we don't have registered -- fails, and the function
      wrapping that call has an explicit `mov r0, #0; bx lr` failure
      path that propagates the failure straight into
      `state = 1`. This is a **real, mechanically accurate**
      "insufficient memory" trigger: the game asks for some subsystem
      via `CreateInstance`, doesn't get it, and (reasonably, from the
      game's perspective) reports it as a resource-availability problem.
      **Not yet fixed**: `0x01002001` is an unidentified real BREW class
      (a different numeric family than `AEECLSID_DISPLAY`'s
      `0x0100_1xxx`, plausibly a Zeebo-specific or sound/config-related
      service) -- registering a stub for it risks a *different* crash
      the moment the game calls a method on it we haven't scaffolded,
      since we don't yet know what interface shape it expects. Tracked
      as the next concrete step: identify (or reference-implement a
      shape for) class `0x01002001`, alongside expanding `font5x7` for
      legible real text now that we know what it's mostly needed for.
      **Cleared the `0x01002001` gate with a deliberately generic
      scaffold, not a guessed real interface**: added
      `core/brew/scaffold_object.h/.cpp`
      (`BuildGenericStubObject(memory, hle, vtable, object, slot_count)`
      -- every slot just returns 0, no assumed shape) plus
      `IDisplayHle::GetDeviceBitmap`/`SetDeviceBitmapInstance` (real
      disassembly of `0x1b5c0` showed the game immediately dereferencing
      `GetDeviceBitmap`'s result's vtable, so leaving it unwritten --
      the previous blind `Stub` behavior -- was a second, independent
      crash risk beyond the `CreateInstance` failure). `tools/game_probe.cpp`
      now registers a 40-slot scaffold for `0x01002001` (sized to cover
      the highest slot, 33, the disassembly shows being called) and a
      20-slot scaffold as the device bitmap. 2 new tests
      (`tests/scaffold_object_test.cpp`: object-header/vtable wiring,
      every slot callable and returns 0;
      `IDisplayHle.GetDeviceBitmapWritesTheRegisteredInstanceAndReturnsSuccess`
      in `tests/brew_test.cpp`). **Verified against the real game**: the
      `applet+0x24` state variable (read via a temporary debug print,
      removed after use) now reads `0` instead of `1` past this specific
      gate -- confirmed via `arm-none-eabi-objdump` that the caller
      (`0x1b154`) now sees a nonzero (success) return from `0x1b5c0`.
      **The chain goes deeper**: the *same* real disassembly technique
      applied to `0x1b5c0`'s caller (`ddragonz.mod` offset `0x1b060`,
      the applet's broader graphics-init routine) shows it gates
      `applet+0x24` on **seven** sequential subsystem-init calls, not
      just the one already found -- two (`0x96f0`, `0x1aea0`) confirmed
      unconditionally successful in this binary, `0x1b5c0` now fixed,
      and three more real gates found and fixed this round:
      - `0x1b2fc`: two more `ISHELL_CreateInstance` calls, for classes
        `0x01001003` and `0x01001014` (confirmed via direct objdump on
        the literal-pool addresses `0x1b394`/`0x1b398`). Neither result
        is dereferenced inside that function itself, so generic
        40-slot scaffolds (registered in `game_probe.cpp`) were enough.
      - `0x1d5b8`: two *more* `CreateInstance` calls, for classes
        `0x01014bc3` and `0x01014bc4`. **A real mistake caught and
        fixed in the same round**: a first attempt reused the
        `0x0100100x`-family class IDs from the `0x1b2fc` gate on the
        assumption they'd repeat -- wrong, and caught immediately
        because the resulting run showed a genuine crash
        (`pc=0x00000000`, "wandered outside module") instead of a clean
        pass/fail, since the real code's own error-cleanup path (at
        `0x1d940`) dereferences a still-null local when
        `CreateInstance` legitimately fails for an unregistered class --
        a real bug in *our* registration, not the emulator. Re-checked
        directly against `objdump` output for the exact literal-pool
        addresses (`0x1d970`/`0x1d974`) the function's own
        `ldr r1,[pc,#N]` instructions reference, found the real values
        (`0x01014bc3`/`0x01014bc4`), and registering scaffolds for
        *those* fixed it: the run now reaches `CreateInstance OK` and
        the event loop with **no crash**, though `0x1d5b8` itself still
        legitimately returns failure (state becomes `3`, not `1` --
        a different failure code, i.e. a different dialog/behavior than
        the "insufficient memory" one, not yet decoded) for a deeper
        reason: a real in-module helper (`0x23e58`) that indirects
        through a global pointer chain (not through the `IDisplay*`
        argument it's nominally called with) to reach some other
        object's vtable slot 4, and returns NULL. **Pinned down exactly
        which object via the same instruction-trace technique** (a
        temporary `trace=true` on the `CreateInstance` call, removed
        after use): `0x23e58` reads a module-global pointer at file
        offset `0x4ccb8` (computed PC-relative, not through the
        static-base table) -- which holds the *exact object our own
        `0x01014bc4` scaffold created* (confirmed live: the trace shows
        it loading our own `0x80017000` object address, then its
        `0x80016000` vtable, then calling slot 4, landing on our own
        generic `Stub` sentinel, which is why it returns 0). So this
        isn't a mystery third object -- it's the *same* class
        `0x01014bc4` we already scaffolded, being asked to do
        real work (its slot 4, called with the original `IDisplay*`
        as `po`) that a uniform "return 0" stub can't satisfy. Searched
        the binary for embedded debug/class-name strings (none found --
        this is a release build) and for corroborating references in
        other real `.mod` files (none available -- only one real game
        binary in `research/games/` this session). Real progress
        requires either finding another real BREW binary/doc that
        identifies class `0x01014bc4`'s actual interface, or reasoning
        from its usage here (slot 4, called with the display as `po`,
        result later validated against a fixed `0x3000`-byte size and
        fed to two more real helper calls) -- deferred rather than
        guessed, since a wrong shape risks a harder-to-diagnose failure
        downstream than the current clean "returns failure" state.
        Tracked as the next concrete step, alongside decoding what
        "state = 3" actually displays.
      **Broke the "0x01014bc4 is unidentified" dead end wide open**: the
      Zeebo SDK package bundled in this repo
      (`research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
      OpenGLES_Extension_1.5.3_...zip`) turned out to be a real,
      unextracted MSI installer -- `7z x Installer.msi` pulled out its
      embedded CAB, `cabextract` on that produced ~46 hashed-name files,
      several of which are real Qualcomm C headers/DLLs. One of them
      (`AEEGL.h`) contains, verbatim: `#define AEECLSID_GL 0x01014bc3`
      and `#define AEECLSID_EGL 0x01014bc4` -- an exact match for both
      literals `0x1d5b8` requires. Even better: this codebase already
      has a complete, previously-tested `GlHle` (real `IGL`/`IEGL`
      implementations, built in an earlier phase for a different purpose
      and wired up directly rather than through `CreateInstance`) --
      `tools/game_probe.cpp` now registers `GlHle::BuildGl`/`BuildEgl`'s
      real objects as the `CreateInstance` answers for these two real
      classes instead of generic scaffolds, and removed the now-
      redundant direct `BuildGl`/`BuildEgl` calls further down.
      **Found two more real, evidence-backed gaps this unlocked** (all
      via the same trace-then-objdump technique, `trace=true` +
      temporary PC-filtered gate prints, both fully reverted before
      each commit):
      - A missing static-base runtime-table slot, offset `0xe8`
        (STRSTR): real disassembly of `0x1d5b8` shows
        `eglQueryString(dpy, EGL_EXTENSIONS)`'s result fed straight into
        this slot alongside a literal string read directly out of the
        real file's bytes (`"EGL_QUALCOMM_COLOR_BUFFER"`, at file offset
        `0x4fcc4`) -- the standard `strstr(exts, "SOME_EXT")` idiom.
        Implemented as real `ModRuntime::StrstrImpl` (3 new tests:
        finds-a-match, no-match, empty-needle). This only crashed
        because `GlHle::EglQueryString` (slot 7 of `IEGL`) was a blind
        `Stub` returning null -- implemented for real: returns an
        honest (never-null, per the real EGL spec's guarantee) string
        for `EGL_VENDOR`/`EGL_VERSION`/`EGL_CLIENT_APIS`, and an empty
        string for `EGL_EXTENSIONS` since no extensions are implemented
        (2 new tests: never-null across several query names, extensions
        string is genuinely empty).
      - The bitmap object `IDisplay::GetDeviceBitmap` returns gets a
        second real call site (beyond the one in `0x1b5c0`): its slot 2
        again, this time inside `0x1d5b8`, in the same
        "QueryInterface"-shaped way (`obj, clsid=0x01001045, &ppo`) --
        and this time the result is unconditionally `Release()`'d
        moments later with no null check, so leaving the output pointer
        unwritten (the generic scaffold's blind-`Stub` default) is a
        real, confirmed crash (traced to a null-pointer `bx`), not a
        hypothetical risk. Added `BuildStubObjectWithOverride` to
        `scaffold_object.h/.cpp` (same generic-stub base, but one named
        slot gets a real caller-supplied implementation) and used it to
        give the bitmap's slot 2 a real (if still generic-downstream)
        `CreateInstance`-shaped implementation: succeeds and writes a
        fresh scaffold object for `clsid == 0x01001045`, fails
        otherwise -- exactly mirroring `IShellHle::CreateInstanceImpl`'s
        own discipline. 2 new tests (override slot runs the real
        implementation; non-overridden slots stay generic).
      **Verified against the real game**: with all of the above, the
      *entire* graphics-init routine at `0x1b060` -- which turned out to
      gate `applet+0x24` on **ten** sequential checks, not seven --
      passes zero-crash from `AEEMod_Load` through `CreateInstance`
      through `HandleEvent(EVT_APP_START)` and into the steady-state
      tick loop, for the first time this project.
      **Found and identified an eighth real, previously-unknown gate**
      (same technique): `0x1b71c`, called from inside `0x1b060` after
      the GL/EGL work, does `ISHELL_CreateInstance(shell,
      ClsId=0x0106c411, ...)` then `IHID_GetConnectedDevices(pIHID,
      nDeviceType=0x0106c3fd, ...)`. Both literals are real and
      confirmed two different ways: they're named exactly
      (`AEECLSID_HID`, `AEEUID_HID_Joystick_Device`) in the real BREW
      SDK sample source already bundled in this repo
      (`research/samples/conftest_source/conftest/GamepadMgr.c`), and
      the real `AEEIHID.h` (found the same way as `AEEGL.h`, in a
      *different* Zeebo SDK installer CAB under
      `research/docs/sdk_installer_extract/sdk_installer_cab/`) confirms
      `GetConnectedDevices` really is vtable slot 7
      (`INHERIT_IQI`'s 3 slots + `CreateDevice`/`GetDeviceInfo`/
      `GetNextConnectEvent`/`RegisterForConnectEvents`/
      `GetConnectedDevices`). We have no real joystick to enumerate, so
      `game_probe.cpp` registers a `BuildStubObjectWithOverride` scaffold
      whose slot 7 honestly reports zero connected devices (true, not
      guessed) -- confirmed via the gate-trace technique that this is
      enough to clear this specific check. A second class the same
      routine would need next (`0x01041207`, only reachable once a
      device is found) is registered as a fully generic scaffold, since
      it's unreached in practice (0 devices) but still needs to exist
      so `CreateInstance` doesn't fail outright if that assumption is
      ever wrong.
      **Found a ninth gate, not yet identified**: `0x1c6b0` (called
      right after the HID work) delegates to `0x22384`, which delegates
      to `0x237c4`, which calls a method (vtable slot 2, args
      `(object, name=<a literal string pointer>, flags=2)`) on the
      object created for `ClsId 0x01001003` back in the `0x1b2fc` gate
      -- our generic scaffold's blind `Stub` returns a null handle,
      which the caller correctly detects and returns failure for (no
      crash, just the same "insufficient" dead end one layer deeper).
      This has the shape of a real resource/texture/font "load by name"
      call, but unlike `AEECLSID_GL`/`EGL`/`HID` above, nothing in this
      repo's bundled research materials (searched: extracted GLES SDK
      cab, extracted plain sdk_installer_cab, the SDK headers reference)
      names class `0x01001003` or corroborates a real shape for this
      call -- implementing it further would mean guessing a return
      value and its downstream consumption, not verifying one, so
      deliberately stopped here rather than fabricate behavior. Tracked
      as the next concrete step.
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
