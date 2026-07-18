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
      Extensive real-disassembly-driven investigation log (every HLE gap
      found and fixed, each grounded in real evidence — never guessed) grew
      large enough to dominate this file. **Moved to `PHASE8_LOG.md`** —
      see that file for the full chronological history; this entry now
      holds only the current summary.

      **Current status**: real per-frame game logic runs with zero
      crashes — `AEEMod_Load` → `IModule::CreateInstance` →
      `HandleEvent(EVT_APP_START)` → the steady-state tick loop all
      complete cleanly against the real `ddragonz.mod`. Along the way:
      real ROPI static-base loading, 11 static-base runtime-table slots
      (MEMCPY/MEMSET/STRCPY/STRLEN/MALLOC/FREE/GETUPTIMEMS/
      GETAPPCONTEXT/bounded-copy/STRSTR/sprintf-family), real key input,
      real `DrawRect`/`SetColor`/glyph rendering, and a ten-gate
      `CreateInstance` init chain — including identifying real classes
      `AEECLSID_GL`/`AEECLSID_EGL`/`AEECLSID_HID` (confirmed via real
      bundled SDK extraction) and very likely `AEECLSID_FILEMGR`/
      `AEECLSID_DIB`/`AEECLSID_SignalCBFactory` (strong circumstantial
      evidence) — are all real, tested, working HLE now.
      **Not yet playable**: the game is currently stuck redrawing a
      diagnostic overlay (`"ERROR CODE:6"` / `"LIST COUNT:3"`), frozen
      since real tick 3. Four real gaps found and fixed so far: (1) the
      game opens its own packed resource archive as a raw file
      (`IFILEMGR_OpenFile("sound.ggz")`), which the dev tool's
      `VirtualFilesystem` never exposed (only each archive's
      *decompressed entries*, not its own raw bytes); (2) a real,
      foundational bug in `FileHle::SeekImpl` — it returned the
      resulting file position instead of `AEE_SUCCESS`/`AEE_EFAILED`
      (confirmed backwards against the real `AEEFile.h` contract),
      silently breaking any real seek to a nonzero position; (3) class
      `0x01001014` (found alongside `AEECLSID_FILEMGR` at the very
      first `CreateInstance` gate this session, real identity still
      unconfirmed) implemented as `FileHle::BuildLastOpenedFileProxy`
      — an evidence-grounded proxy whose `Read` always forwards to
      whichever file was most recently opened, rather than the old
      blind scaffold that silently read 0 bytes every time; (4) the
      emulated heap (arbitrarily sized at 1MB) bumped to 16MB after
      real disassembly showed the resource loader legitimately
      exhausting it via real per-item `MALLOC` calls. With all four
      fixes, the loader now gets through dozens of real resources
      (previously 0–1) before still ultimately failing — traced the
      new failure to the real, final (74th of 74) entry in
      `sound.ggz`'s own GGZ table: its declared decompressed size
      (1034 bytes) doesn't fit in the real file's remaining bytes
      (505) after that entry's offset, because the real game code
      reads raw, undecompressed bytes directly off disk and keeps
      pulling from whatever comes *after* the current entry to satisfy
      its requested total — which only works because every other
      entry has more archive data after it. Independently verified
      (Python `zlib`) that those exact 505 real bytes are a complete,
      valid gzip stream decompressing cleanly to exactly 1034 bytes —
      not truncation or a parsing bug on our end. Also disassembled
      the dispatcher itself directly: `"LIST COUNT"` and the per-tick
      case index turned out to be the *same* struct field (corrects an
      earlier entry in `PHASE8_LOG.md` that treated them as separate),
      and a live watchpoint across ~900 subsequent ticks showed zero
      further writes to it or the error-code field after tick 3 — this
      isn't an infinite retry loop, the game gives up after exactly one
      real attempt and stops invoking this subsystem entirely; the
      frozen diagnostic is just stale memory redrawn by an unrelated
      render path. Also confirmed via disassembly that the loop's
      81-slot bound is a hardcoded literal, not derived from any real
      parsed count. Current best hypothesis: this repo's `sound.ggz`
      research asset may be missing trailing bytes the real distributed
      file has (which would let entry 74's raw read spill into further
      archive data the way every earlier entry's does) — i.e. likely a
      research-asset gap rather than an emulator gap, though not yet
      confirmable without another real copy of the file. See
      `PHASE8_LOG.md`'s final entries for the full trace and reasoning.
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
