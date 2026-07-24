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
        instructions, LDM/STM's user-bank/exception-return (S=1) variant.
        Will be picked up incrementally as real game code needs them.
        (Thumb state itself — full T16 decoding plus ARM/Thumb
        interworking — was deferred here too originally, but has since
        been implemented; see Phase 8's Peggle entry below.)
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
- [x] Handle whatever asset sub-formats appear inside GGZ containers for the
      target test game (models, sprites, etc. — format specifics are a
      per-content research task, informed by `ggzbrewtools`' documented
      coverage) — was deferred, not blocking this phase's exit criterion;
      `.obm1` (every one of Double Dragon's 89 real sprite/texture assets)
      cracked in Phase 8 once real further progress needed it — see
      `core/loader/obm1.h` and PHASE8_LOG.md. `IFile`/`IFileMgr` themselves
      needed no changes: they already hand back raw bytes correctly
      regardless of what those bytes mean internally.

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
      **Superseded**: the "frozen since tick 3" state above was reached
      using ClsId `274754` (Double Dragon's download-catalog folder
      number) at the very first `IModule::CreateInstance` call — later
      found, while investigating Super BurgerTime, to not be the real
      value the module's own code checks at all (see this file's Super
      BurgerTime section and PHASE8_LOG.md). The real value is
      `0x0102f789`; with it, `game_probe` reaches **"Reached the event
      loop with no unhandled instruction!"** cleanly, same as the other
      two titles, superseding this entire earlier trace (not yet
      re-driven forward from there this round). Separately, Double
      Dragon's real sprite/texture format — `.obm1`, all 89 assets in
      `data.ggz` — is now fully cracked (`core/loader/obm1.h`,
      PHASE8_LOG.md): confirmed via a legible decoded font sheet and a
      complete, correct character animation sheet. Not yet wired into
      any real render path — the next concrete step is letting the
      interpreter run further past the event loop with the correct
      ClsId and seeing whether real code reaches and correctly executes
      its own real `.obm1`-decoding/texture-upload logic.
      **Did exactly that, and found two real, foundational bugs**: (1)
      `IDisplayHle::DrawText` assumed 16-bit UTF-16 AECHAR; real Zeebo/
      BREW AECHAR is a plain 8-bit byte — silently garbling every text
      draw in every title tested so far, not just this one. (2) ClsId
      `0x01014bc4` (`AEECLSID_EGL`, already correctly registered against
      the real `GlHle` EGL object) was being silently overwritten by a
      generic-stub registration for the *same* ClsId added later,
      investigating Peggle — a dead-code fallback path that never
      actually helped Peggle (its own primary class is itself an
      always-succeeding stub) while breaking Double Dragon's real,
      direct EGL init. Together these caused a real, legible
      "Failed in the initialization of the library." dialog Double
      Dragon was silently stuck showing since real tick 0. Fixed both;
      the dialog no longer triggers, and real code now opens real files
      for the first time all session — `sound.ggz` and the real
      `./udata/ddz.sav` save flow, both previously documented as
      real-but-unreached call shapes. Also fixed the project's own
      `hello_brew` test fixture, which shared the same 16-bit AECHAR
      assumption. 259/259 tests pass. No regression on Peggle/Super
      BurgerTime. See PHASE8_LOG.md for the full evidence trail
      (including a live memory watchpoint tracing the dialog's real
      `applet+0x24` status field back through the real call chain to
      the exact failing `eglGetDisplay` call).
      **Confirmed the fix's real impact and found the next wall**: real
      code now genuinely streams through `sound.ggz` end-to-end for the
      first time (a real backward walk through its GGZ table, real
      variable-sized compressed-audio reads), completing within the
      first few ticks, then settles into a new steady state that
      neither more real time (90s, no change) nor a full sweep of
      every real AVK key code this codebase's dispatcher recognizes
      unblocked. That sweep was inconclusive, not a dead end — either
      the real "continue" input isn't in that code range or the gate
      isn't input-shaped at all; needs real disassembly tracing to
      answer, same method that found the EGL bug, not more guessing.
      Not attempted this round. See PHASE8_LOG.md.
      **Did that tracing.** The stable title-screen state gates on one
      bit (`0x100`) of a real word (`applet+0x361c`) recomputed every
      tick as the OR of two fields, themselves copied every tick from a
      real per-input-source struct at `applet+0xa20` (64-byte stride,
      2 sources) — and that struct is itself cleared (real `memset`)
      every tick, presumably meant to be refilled from a real input
      poll this codebase doesn't drive. `HandleEvent`'s own real key
      dispatch (confirmed: itself just a trampoline through a real,
      data-driven `applet+24` pointer) writes into a *different* pair
      of fields (`applet+0x28`/`+0x2c`/`+0x30`) that never feed this
      gate at all — so the AVK-key path from two rounds ago was never
      going to work, not a matter of trying more codes. Real root cause
      (what actually populates `applet+0xa20` — a distinct real input/
      touch/HID subsystem, given Zeebo is a 2009 touch device not a
      classic AVK keypad, is the live hypothesis) not yet found; needs
      a watchpoint on that struct directly, not yet done. Well-scoped
      next thread. All instrumentation reverted; 259/259 tests pass
      (no functional changes this round). See PHASE8_LOG.md.
      **Found it, and it's conclusive: not a bug.** A direct
      watchpoint on `applet+0xa20` caught a real `ISHELL_CreateInstance
      (cls_id=0x106c411)` call -- `AEECLSID_HID`, the real gamepad
      class -- writing its object pointer straight into this struct.
      The whole pipeline traced last round reads real per-controller
      HID state; this project's own `hid_obj` scaffold honestly reports
      zero connected devices (we have no real joystick hardware), so
      the gate's bit can never legitimately become set through this
      path. Real Double Dragon appears to gate this specific
      title-screen transition on an external/Bluetooth gamepad, the
      same way real BREW titles could support optional peripherals --
      a genuine hardware dependency, not an emulation gap. Moving past
      it now needs a deliberate choice (teach the HID scaffold to
      report a fake connected controller for testing) rather than more
      tracing; not attempted this round. See PHASE8_LOG.md.
      **Made that choice.** Reporting a connected device unlocked two
      real, previously-latent bugs (both fixed): `IHID_CreateDevice`
      (real vtable slot 3) was a blind stub leaving its output
      `IHIDDevice*` null -- the same unchecked-`CreateInstance`-style
      pattern this project has hit repeatedly -- and the device
      scaffold it now returns needed 40 vtable slots, not this file's
      10-slot default, since real code calls the device's own slot 11.
      Also registered a third real class (`0x01005511`) only reachable
      through this path. `CreateInstance` completes cleanly again and
      reaches the event loop. The title screen still doesn't advance,
      but for a fully expected reason now: a connected-but-idle
      controller correctly reports no buttons held. Simulating an
      actual button press (capturing and directly invoking the real
      registered callback, address `0x11beac`, already seen live) is
      the next well-scoped step, not attempted this round. No
      regression on Peggle/Super BurgerTime; 259/259 tests pass. See
      PHASE8_LOG.md.
      **Turns out what's currently on screen isn't the title screen at
      all — a separate, parallel real blocker.** With the joystick
      connected but idle, real code shows a genuine "CARREGANDO..."
      spinner, then a persistent "LOAD ERROR"/"ERROR CODE:6" dialog.
      Traced it end-to-end (live watchpoints and a temporary
      `OpenFile`/`Read`/`Seek` trace, all reverted) to a real per-tick
      resource loader whose 74-entry `sound.ggz` GGZ header table is,
      by its own internal accounting, short: entries 68-73 (the last
      6) declare more data than the 1,928,097-byte file actually
      contains — entry 73 (`bgm_9.mid`) is missing 529 of its declared
      1034 bytes. Confirmed via a standalone script reading the raw
      file, no emulation involved. This refines, but doesn't resolve,
      the open question from the Peggle section below: a byte-for-byte
      SHA-256 match against an independent archive.org copy already
      showed this `sound.ggz` is the authentic shipped file, not a
      research-asset gap — so the truncation is baked into the real
      asset itself. Two live, undistinguished hypotheses: either this
      project has an unfound real gap that makes it reach this entry
      when real hardware wouldn't, or the retail build genuinely ships
      with this defect and real hardware papers over a short read
      somehow. Not resolved this round — a deliberate stopping point,
      not a guess. 259/259 tests pass (investigation only, no
      functional changes). See PHASE8_LOG.md.
      **Found a third, independent source — points away from "shipped
      defect."** Archive.org item `Zeebo` ("OpenZeebo" compilation, a
      different curation than `zeebo-arquivista`) has its own Double
      Dragon dump (`274754.7z`); pulled just that entry via HTTP range
      reads instead of the full 653MB zip. All three real asset files
      (`ddragonz.mod`/`data.ggz`/`sound.ggz`) are SHA-256-identical to
      this repo's copies — a third independent match, making a bad
      download effectively impossible. Also found that Tuxality's
      independent, closed-source Infuse emulator is on record reaching
      a **playable** state on Double Dragon (May 2025), almost
      certainly against this same public dump, since no other is known
      to exist. A correct implementation evidently doesn't get stuck
      on this LOAD ERROR using these exact bytes — favors "real
      Zeebulator gap" over "shipped defect," though not proven (Infuse's
      source isn't inspectable). Next concrete step: find what should
      stop real code from ever needing GGZ entry 73's full 1034 bytes
      in the first place. Not attempted this round; no repo files
      touched (comparison files stayed in scratchpad). 259/259 tests
      pass. See PHASE8_LOG.md.
      **Did that, and it closes the thread: there's nothing to fix.**
      Disassembled the real static table backing the op-code-2 preload
      loop (`0x11c964`, called from `0x11c248`) — a genuine compiled-in
      constant array in `ddragonz.mod` at `0x14e1cc`, 81 entries, real
      GGZ resource indices. All 74 distinct indices (0-73) appear at
      least once, entry 73 (`bgm_9.mid`) among them, and the loop is
      strictly all-or-nothing (any single failure aborts the whole
      batch, no per-slot skip exists anywhere in the function). This is
      an unconditional requirement baked directly into the compiled
      game code: no correct interpreter can run this exact binary
      against this exact file without hitting this failure. Combined
      with three independent byte-identical public copies of
      `sound.ggz`, the most parsimonious explanation is that the
      original real-hardware capture of this file — whatever tool
      first dumped it, long before any preservation effort — itself
      stopped a few hundred bytes short, and every public copy since
      has propagated that same short capture. Not a Zeebulator gap;
      moving past this dialog needs a more complete `sound.ggz` than
      any public source currently provides, not a code change. No
      further sourcing attempted (already searched once this round per
      this project's ask-first practice). 259/259 tests pass (no
      functional changes — read-only disassembly and a standalone
      script only). See PHASE8_LOG.md.
      **Reopened after being asked directly to match what a real,
      independent emulator (Infuse) does — and that pushed this all the
      way to a real, working fix.** Ran Infuse's actual Linux binary
      against this repo's own exact asset files; confirmed via `strace`
      and a live screenshot that it hits the identical short read on
      `sound.ggz` and still reaches the real splash screen. Chasing why
      required real disassembly of `0x11bfd0` (the actual per-entry
      reader, not `0x10739c` as assumed before): it's a plain
      accumulate-until-`length`-or-EOF raw byte copy with **no
      decompression at that level**, needing the file to physically
      contain each entry's full declared length — even though the same
      bytes are genuine, valid gzip streams (confirmed decompressible
      standalone) that this project's own separate `core/loader/ggz.*`
      already handles correctly for other purposes. **Fix**: `tools/
      game_probe.cpp`'s `MergeGgzInto` now zero-pads the raw archive
      blob (never the individually-extracted, correctly-decompressed
      entries) out to the largest extent its own header table declares,
      whenever the real file falls short. Verified: `list_count` now
      reaches 3 with `error_code` staying 0 (previously 6); real
      execution runs measurably further before hitting a new,
      different, out-of-scope gap (an unimplemented MRS/MSR
      instruction) — a distinct next thread. Scoped narrowly enough
      that Peggle/Super BurgerTime (no GGZ format, unaffected either
      way) can't regress from it. 259/259 tests pass. See PHASE8_LOG.md.
      **Chased that MRS/MSR crash and found two more real gaps, both
      fixed — Double Dragon now runs a full 10 seconds with zero
      crashes for the first time all session.** Extended the existing
      "wandered outside the module" diagnostic to also report the last
      real pc/lr before the jump (permanent, kept). That pointed at a
      real call through a 19th, previously-unfound static-base table
      slot (offset `0xdc`) — added as another safe no-op, same as the
      18 already confirmed. Took real execution from 479 to 111,400
      steps before a second null-pointer crash: a genuine `IShell`
      vtable call at slot 43, one past this project's previously
      "verified against real Qualcomm source" 42-slot (0-41) count.
      Extended the vtable with generously-sized, clearly-unconfirmed
      stub headroom (through slot 49) rather than guess what's really
      there. After both fixes: zero wander warnings, zero thrown
      exceptions, for the full run. A temporary DrawText trace
      (reverted) confirmed the LOAD ERROR dialog is completely gone —
      only the real "CARREGANDO..." spinner shows now, still
      legitimately loading rather than stuck. 259/259 tests pass. See
      PHASE8_LOG.md.
      **Confirmed loading now genuinely completes** (list_count reaches
      13 — the real last entry, flagged terminal — with error_code=8, a
      real "done" status, not a failure) and mapped the real HID
      button-press delivery pipeline end-to-end using a bundled real
      Qualcomm reference sample (`research/samples/conftest_source/
      conftest/GamepadMgr.c`) and the real `AEEIHIDDevice.h`/
      `AEEHIDButtons.h` headers (found this round under
      `research/docs/sdk_installer_extract/sdk_installer_cab/`).
      Corrected an earlier round's callback address (`0x11beac` is
      actually the device-connect callback, not button; the real one is
      `0x11bdf4`) and implemented real `ISignalCBFactory::CreateSignal`/
      `IHIDDevice::RegisterForButtonEvent`/`GetNextButtonEvent` plus a
      tick-loop injector that queues real button-press events and
      invokes the real captured callback directly. Found and worked
      around a real trap along the way (queuing Start/HOME aborts the
      real event loop before later events are processed — confirmed via
      disassembly, not guessed). Verified the full pipeline works:
      simulated presses for all 4 real action buttons + d-pad correctly
      set the real per-button bitmask. The title-screen gate
      (`applet+0x361c`) still didn't open, though — it reads from a
      different real struct (`applet+0xa20`) than the one this
      confirmed-correct button state lands in, and what copies one into
      the other isn't found yet. A concrete, narrower next thread. All
      temporary diagnostics reverted; the real signal/button
      implementation kept as a permanent, documented addition. 259/259
      tests pass. See PHASE8_LOG.md.
- [ ] Validate the HLE against a second real game (Peggle), started this
      round to check whether Double Dragon-tuned HLE generalizes.
      Downloaded 61 real Zeebo titles from the `zeebo-arquivista`
      archive.org preservation item (see PHASE8_LOG.md for provenance;
      files live under `research/games/_archive_org_zeebo-arquivista/`,
      git-ignored) — re-downloading Double Dragon from it turned out to
      be byte-for-byte identical to the copy already in this repo,
      independently confirming that file is authentic and complete, not
      a truncated research-asset gap (see the entry above). Surveyed all
      61 titles' formats: **only Double Dragon uses the GGZ archive
      format** — every other title uses a different container per
      publisher/engine (classic-arcade ports like Bad Dudes/Caveman
      Ninja/Pac-Mania wrap what looks like an embedded arcade-emulation
      core with its own `"PACK"`-magic `.pkg` format; several PopCap
      titles — Peggle, Bejeweled Twist, Zuma's Revenge — use a cleaner
      single-archive `resources.bar`/`resources.dat`, format not yet
      identified, doesn't match public PopCap BAR headers). Picked
      Peggle for its clean layout and smaller `.mod` (274KB vs Double
      Dragon's 462KB). Found Peggle's own real `IModule::CreateInstance`
      ClsId — `0x01099CD6` / `17407190`, unrelated to Double Dragon's
      `0x0102F789` — confirmed directly against `peggle.mod`'s own raw
      file bytes at the literal-pool address `CreateInstance` compares
      against (same technique as Double Dragon's). With that and the
      real static-base DBGPRINTF slot fix (offset `0x9c`, committed — see
      `core/brew/mod_runtime.h`), `CreateInstance` now runs real code
      to completion and returns a real, non-null applet pointer.
      **Implemented Thumb (T16) decoding and ARM/Thumb interworking**
      (`core/cpu/arm_interpreter.{h,cpp}`, 33 new tests in
      `tests/thumb_test.cpp`) after `HandleEvent(EVT_APP_START)` hit a
      real `BX` into Thumb code the interpreter couldn't execute at all
      — Double Dragon's own `.mod` apparently never needed it. Covers
      all 19 real Thumb1 instruction formats and real ARM/Thumb
      interworking (BX/BLX in both states, Thumb `POP{pc}`, ARM
      `LDR`/`LDM` into `pc`). Verified against real Peggle code:
      `HandleEvent` now runs tens of thousands of real Thumb
      instructions cleanly. That, plus two more real static-base slots
      found the same way as every other gap in this project (offset
      `0x44`, a second MEMCPY-equivalent; see `core/brew/mod_runtime.h`),
      pushed real execution to ~25,800 steps before hitting a new, deeper
      gap: a third real field (offset `0x2c`) on the shared "app
      context" struct (the same struct the confirmed offset-`0xc0` slot
      returns) that real code dereferences expecting an actual object,
      not the zero it currently holds there. **Fixed**: added a general
      mechanism for this real ABI variant
      (`scaffold_object.h`'s `BuildGenericRelativeVtableStubObject`,
      since it's a genuine ROPI relative-vtable ABI pattern, not a
      one-off) and a third settable `ModRuntime` context field
      mirroring Shell/Display — the real interface itself is still
      unidentified, wired to a safe no-op placeholder for now. That
      unblocked one more real gap: static-base offset `0x74` is
      REALLOC, confirmed via two independent real growable-array call
      sites (different element sizes, same `(old_ptr, new_size)`
      contract). **With both fixed, `HandleEvent(EVT_APP_START)` now
      completes successfully and Peggle reaches its real steady-state
      event loop** — the same milestone Double Dragon reached, on a
      second real game. See PHASE8_LOG.md for the full trace and
      reasoning, including a detour where a long stretch of no visible
      output was first mistaken for a hang before being confirmed as
      the tool's own correct, by-design infinite event loop.
      Ran Peggle for many further real ticks (thousands, over 60+ real
      seconds) with no new crash — confirmed indirectly (the process
      needed external termination rather than exiting on its own, which
      only happens on success; any real error sets `running=false` and
      returns cleanly). Started reverse-engineering `resources.bar`:
      found via real disassembly that it's opened through the real
      `ISHELL` vtable (not a custom parser), and cross-referencing the
      call's exact register arguments (`RESTYPE_BINARY=0x5000`, a
      buffer of `-1`) against the real bundled `AEEShell.h` identifies
      it precisely as `ISHELL_GetResSize`/`IShell_LoadResDataEx` — i.e.
      `resources.bar` is a **standard BREW application resource file**
      (`AEE_RES_EXT`), not a Peggle-specific format. Its real *binary*
      layout is still uncracked, though: unlike every other format in
      this project, its reader lives in the real device's own
      OS/firmware, not in any `.mod` we can disassemble, so cracking it
      needs blind, evidence-anchored byte analysis instead — deliberately
      not guessed further without a known (resource ID → size) pair to
      verify against first (the one real call site found isn't reached
      by blindly driving ticks; needs its own investigation for how
      Peggle actually reaches it).
      **Found why, and it's a real, significant finding of its own**:
      traced `ISHELL_SetTimer` directly (a live print inside
      `IShellHle::SetTimerImpl`, reverted after use) and confirmed it is
      called **exactly once** across the entire run, registering a
      20ms, plain-ARM-mode (bit 0 clear, ruling out a suspected Thumb-
      interworking bug in how `tools/game_probe.cpp` dispatches timer
      callbacks) callback — and that callback's own execution (already
      fully traced earlier: ~24 real instructions, one `GETAPPCONTEXT`
      call, a clean `bx lr` return) never calls `SetTimer` again to
      re-arm itself. Double Dragon's whole per-frame loop depends on
      exactly that self-rearming pattern (see this file's own real,
      confirmed doc comment on `IShellHle`).
      **Found and fixed why**: the callback's whole re-arm path is
      gated behind a fourth real field on the shared "app context"
      struct (offset `0x24`, `SetFourthContextObject()`) that this
      codebase never wrote, so the gate always failed. Wired to a
      real, writable, zeroed memory block with just the one confirmed-
      load-bearing field (`+20`) pre-set non-zero. Verified: tick 0
      now runs hundreds of real HLE calls (including a real
      `ISHELL_CreateInstance` for the same `FileMgr` class Double
      Dragon uses) instead of one.
      **Immediately hit a new, bigger-picture gap**: real code treats
      `context[0x24]` not as a small object but as the base of a
      **large global data arena** — different subsystems reach their
      own portion of it via large fixed offsets (e.g. `+0x45000`) from
      that same base. Traced precisely (confirmed the resulting null
      pointer, not a bug in the fix), but deliberately not guessed
      further: unlike the narrow `+20` flag, this is open-ended —
      no way yet to know how many more such offsets exist or what real
      data belongs at them. See PHASE8_LOG.md for the full trace and
      the two candidate ways to proceed.
      **Provisioned that arena field and fixed two more real gaps in a
      row**: a write-timing bug (real `HandleEvent` code resets the
      field to zero once during its own init, so the placeholder must
      be written *after* `HandleEvent` returns, not before), and a
      recurring real QueryInterface-style out-pointer chain (caller
      passes an output pointer, ignores the returned status, and
      dereferences whatever was written there — confirmed recurring
      through multiple freshly-returned objects). Generalized the fix
      into an experimental **self-propagating stub** (a recursive
      lambda in `tools/game_probe.cpp` that lazily builds a fresh child
      object for any such out-pointer call, arbitrarily deep) rather
      than hand-patching each level — deliberately kept local to the
      probe tool, not promoted to general scaffolding, since this
      chaining shape is only confirmed at this one real call site so
      far. **Verified**: tick 0 now reaches real
      `ISHELL_CreateInstance(ClsId=0x01001003)`, many real `Seek`-shaped
      and other real HLE calls, and the self-propagating chain itself
      firing through several more real traps — a large jump in real
      execution depth. Hit what first looked like a new, third real
      object convention at the next level (a flat struct with a
      function pointer read directly off a fixed offset, not through a
      vtable) — left undoctored rather than guessed at.
      **That turned out to be a misdiagnosis of a bug in this
      codebase's own stub, not a real object convention**: closer
      register-level tracing of the exact real call chain showed real
      vtable slot 2 does use the assumed `(this, id, ppOut@r2)` shape,
      but real slot 3 uses a *different*, also-real shape —
      `(this, ppOut@r1)` — and other real slots (and one real call
      site) pass no output pointer at all, with `r1`/`r2` holding
      leftover garbage or explicit zero. The old stub blindly wrote a
      child object into r2 for every slot regardless, corrupting
      whatever it found there — once, real address 0 itself — and it
      was real code reading back that self-inflicted corruption that
      produced the earlier "flat struct" illusion. **Fixed** by only
      special-casing slots 2 and 3 with their real, evidenced output
      registers (skipped when null), leaving every other slot a plain
      side-effect-free stub. **Verified**: tick 0 now makes 337 real
      HLE calls (up from 207) and survives 6312 real ARM steps (up
      from 3155) before its next wander — roughly double the real
      execution depth. Still eventually wanders to a null pointer from
      a new, not-yet-individually-traced call site. See PHASE8_LOG.md
      for the full trace evidence; continuing to chase the new wander
      point the same way is the next concrete step.
      **Chased that new wander point and it was a real gap this
      codebase already knew how to fill**: real code at `peggle.mod`
      offset `0x132dfc` reads a **fifth confirmed field on the shared
      app context struct, offset `+0x28`**, with no null check, and
      calls through it using the exact same ROPI relative-vtable
      convention already implemented for the third field (`+0x2c`).
      Added `ModRuntime::SetFifthContextObject` (an exact mirror of
      the third field's setter) and wired it to the same kind of
      relative-vtable scaffold. **Verified — this is the milestone the
      whole Peggle investigation has been chasing**: the timer callback
      now runs tick after tick with zero wander warnings and zero
      thrown exceptions, confirmed both via ten traced clean ticks and
      a 60-second unbounded run that needed external termination
      rather than exiting on its own — the same "success looks like a
      hang" signature already trusted for Double Dragon's own steady
      state. This is *sustained* execution, not necessarily *correct*
      execution yet: most vtable slots on every placeholder object
      involved are still safe no-ops, and nothing has driven visible
      output to the window. The next concrete step is determining
      whether real game state (level/resource loading, a rendered
      frame) is actually progressing over many ticks, or just looping
      harmlessly on placeholders. See PHASE8_LOG.md for full evidence.
      **Checked directly (temporary instrumentation, reverted): it's
      looping harmlessly.** A 30-second real run fired only five real
      "does something visible/external" events (`DrawText`/`DrawRect`/
      `Update`/`SetColor`, file opens, unknown-class `CreateInstance`
      requests) total, all during one-time startup, none afterward —
      zero draws the entire run, zero further file opens (including no
      attempt at `resources.bar`). Diffing the full per-tick HLE trace
      between tick 1 and tick 5 confirmed it: the exact same 20 calls,
      same order, same arguments, every tick. The sustained execution
      is real, but it's a fixed loop over placeholder objects, not real
      game logic — most likely polling one of the still-unidentified
      interfaces (the third/fifth context fields, or the fourth field's
      arena beyond its one confirmed sub-offset) that our safe no-op
      stubs can never report "ready," so it never falls through to real
      resource loading or rendering. Next lead: a real, literal ID
      constant baked into the module at the confirmed slot-2
      QueryInterface call (`0x0101eb0b`) plus two unknown real `ClsId`s
      surfaced this round — cross-referencing these against real BREW/
      Zeebo headers or other `.mod` binaries in `research/` may reveal
      what real interface these placeholders should actually be. See
      PHASE8_LOG.md for full evidence.
      **Chased that lead**: no header match for any of the three IDs in
      this repo's (small, 13-file) reference BREW header subset, but a
      binary search across every real `.mod` in `research/games/` found
      ClsId `0x0103d8ec` is **not Peggle-specific** — the exact same
      real `ISHELL_CreateInstance(0x0103d8ec)`-then-fallback-to-
      `0x01014bc4` instruction sequence, both literal IDs included,
      appears verbatim in Super BurgerTime's own `.mod` too — strong
      evidence of a real, standard SDK-emitted helper. Registered
      generic scaffolds (the same established, deliberately-unguessed
      treatment as the earlier `0x01002001` case) for this pair plus a
      third real ClsId (`0x01030766`, traced separately). **Verified**:
      tick 0's call count dropped as expected (fallback path now
      skipped since the primary succeeds), but the steady-state per-
      tick loop is byte-for-byte unchanged — **ruling out all three as
      the per-tick blocker**. The loop's own real ID (`0x0101eb0b`)
      still has no header match; identifying it is the next concrete
      step, likely needing either a fuller real BREW MP header set or
      more structural tracing. See PHASE8_LOG.md for full evidence.
      **Web search for `0x0101eb0b` and the other unidentified IDs came
      up empty** — no public source has any of them. A promising-
      looking lead, the closed-source third-party "Infuse" Zeebo
      emulator, turned out to have no referenceable code (proprietary,
      no-redistribution license) and doesn't target these games anyway;
      no public Zeebo firmware/system dump was found anywhere either.
      This specific lead is exhausted.
      **Took the structural-tracing option instead**: a full
      instruction trace of one steady-state tick found the very first
      real action every tick is reading a sibling arena field,
      `context[0x24]+0x45000+0x3dc` (next to the already-provisioned
      `+0x3d8`), and passing it un-null-checked into a real subroutine
      that dereferences it repeatedly — with it left at 0, this was
      confirmed writing a real per-tick counter to real address 4 and
      reading real address 0 back, i.e. genuine memory pollution, not
      simulated behavior. This field's own real layout looks like
      Peggle's own internal per-tick game-object data (not a generic
      BREW interface) and wasn't safe to guess at, so it got the same
      conservative treatment as the fourth field's own arena
      allocation: a real, isolated, zeroed memory block. **Verified**:
      the real accesses now land on that isolated block instead of real
      low memory; steady-state behavior is otherwise unchanged (still
      the same do-nothing branch every tick) — a hygiene fix, not a
      progress unlock. 241 tests pass; Peggle remains stable.
      **This is a reasonable pause point for this investigation
      thread**: the per-tick loop is now real, evidence-traced, and
      free of known memory pollution, but still doesn't progress past
      its fixed steady state. Further progress needs either a real
      BREW MP SDK header dump this project doesn't have access to, or a
      much larger, Peggle-specific reverse-engineering effort into its
      own per-tick game data — both bigger asks than the incremental
      fixes made so far. See PHASE8_LOG.md for full evidence.
- [ ] Validate the HLE against a third real game (Super BurgerTime),
      started after pausing the Peggle-specific investigation above —
      untapped territory, and a useful check that the HLE core
      generalizes rather than being overfit to two titles. Ships as one
      of the classic-arcade ports: loose per-asset files plus a
      `"PACK"`-magic `.pkg` container, a third real asset-container
      shape distinct from both prior titles (GGZ, `resources.bar`) —
      not yet cracked (deliberately not assumed to be a byte-exact
      Quake PAK just because of the magic-byte coincidence; the actual
      directory-offset fields don't parse coherently under that
      assumption), deferred until asset loading is actually reached.
      **Found and fixed a real, foundational CPU gap**: the entire
      ARMv6 "Extend" instruction family (SXTB/SXTH/UXTB/UXTH + their
      accumulate forms) was unimplemented — the very first real
      instruction Super BurgerTime executes beyond the common
      `AEEMod_New` prologue is a real `uxth r0, r0`. Confirmed the real
      encoding empirically (assembled each mnemonic, read back the
      actual bytes) rather than from memory, and implemented the whole
      closely-related family in one pass. **Verified**: `AEEMod_Load`
      now runs 742,000+ real steps past the previous immediate failure.
      **Hit a new, much deeper wall immediately after**: a real function
      returns via the APCS `ldm sp,{fp,sp,pc}` stack-frame convention,
      and the popped return address is `0` — the same "wander outside
      the module, coincidentally re-enter from the start" pattern
      already seen for Double Dragon/Peggle, except this time it's a
      CPU/stack interaction gone wrong deep inside the module's own
      compiled prologue, before any HLE surface is reached — a
      materially different kind of gap than any fixed so far. Not yet
      root-caused; tracked as the next concrete step. See PHASE8_LOG.md
      for full evidence.
      **Root-caused precisely, down to the exact mechanism** (temporary
      watchpoints, all reverted): a real ARM ROPI relocation-fixup loop
      (module offset `0x100040`-`0x100054`) correctly processes its
      real, 82,480-entry fixup table once — then a separate real
      "clear it" loop zeroes the table's own memory — then the *same
      fixup loop runs a second time* over the now-zeroed range, and
      every "entry" reading back as `0` makes every iteration
      degenerate to the same target address (its own relocation base),
      corrupting real code there a little further on each of ~82,480
      iterations. That's what corrupts the `uxth`/`mov ip, sp`
      instruction before it executes, which cascades into the garbage
      `fp` and the eventual null return. **Why the fixup loop runs
      twice isn't resolved** — every instruction in the loop re-verified
      correct against direct memory reads; finding the loop's *caller*
      (comparing `lr` at both entries) is the concrete next step. This
      is the deepest and most different kind of gap found in this
      entire investigation across all three titles — real, correctly-
      emulated CPU execution hitting a self-inflicted data corruption
      bug in the module's own relocation logic, not a missing HLE call
      or CPU instruction. See PHASE8_LOG.md for the full mechanism.
      **Correction: that framing was premature.** A one-shot watchpoint
      on the very first corrupting write found it happens during the
      game's first, ordinary pass through its own relocation loop, not
      a replay. **The real cause: this tool's emulated stack pointer is
      a fixed `kBase + 0x200000` offset that safely cleared Double
      Dragon's and Peggle's much smaller `.mod` files, but lands
      *inside* Super BurgerTime's 2.8MB `.mod` — specifically inside
      the exact address range its own real relocation-fixup table
      occupies.** Reading that table off this tool's empty stack
      returns zero mid-walk, and the loop's own real logic degenerates
      into repeatedly corrupting its own relocation base (the `uxth`
      instruction) before it executes — one root cause explaining every
      symptom from both entries above. **Fixed** by sizing the stack
      offset relative to the real module (`kBase + mod_size +
      0x200000`) so it can't collide with any module regardless of
      size. **Verified**: `AEEMod_Load` now completes cleanly,
      `CreateInstance` succeeds too, and execution reaches 40,095 real
      steps into `HandleEvent(EVT_APP_START)` before a new, unrelated,
      much later gap (a coprocessor instruction/SWI encoding at module
      offset `0xa0`). No regression on Peggle or Double Dragon; 250/250
      tests pass. See PHASE8_LOG.md for full evidence.
      **Two more static-base slots found and fixed in quick
      succession** (same "unwritten table slot → null function pointer"
      shape as all fourteen already confirmed): slot `0x40` (called
      `(applet_ptr, a 512-byte stack buffer, size=0x200)`, no confirmed
      match) and slot `0xc` (reached via a standalone trampoline, sits
      in the same cluster as `MEMCPY`/`MEMSET`/`STRCPY`/`STRLEN`, a
      plausible `STRCAT`/`STRCMP` sibling but not confirmed) — both
      registered as safe no-ops. **Verified**: each advances real
      execution measurably (40,095 → 40,177 → 40,254 real steps).
      **Hit a new, differently-shaped wall right after**: an `S=1 with
      Rd=R15 (SPSR restore)` exception at `pc=0x3000000b`, an address
      far outside any real range — some upstream register computation
      produced outright garbage rather than the usual clean null.
      **Traced back precisely: it's the same clean null-pointer wander
      as always, but the root cause is a materially different kind of
      gap.** The null itself comes from `r2` (dereferenced two calls
      deep) being read from a real module global, `0x2e28fc`, that
      falls *inside* the relocation table's own reclaimed scratch
      range — real code expects some other, not-yet-executed real
      initialization to have turned that memory into real BSS/heap
      data by now, and nothing in this codebase's current execution
      path does. Unlike the sixteen static-base slots (missing
      *system* function pointers, safely stubbable), this is a missing
      piece of the *game's own* init sequence — guessing a placeholder
      value here risks silent wrong behavior, not a clean crash, so
      deliberately not guessed at. Finding what real code should
      populate it (likely needs tracing `CreateInstance`'s own body,
      not yet disassembled this deep) is the next concrete step.
      **Followed that lead**: `CreateInstance`'s own real body, traced
      directly, does completely ordinary real work with the guessed
      ClsId (a real class-dispatch lookup, real applet-struct
      construction, a genuine `ISHELL_CreateInstance(0x01001001)` call
      — the same real `AEECLSID_DISPLAY` Double Dragon confirmed) —
      looks like a real, matched code path, not a silently-accepted
      wrong guess. A memory watchpoint spanning the entire run confirms
      `0x2e28fc` is written to exactly twice, both during `AEEMod_
      Load`'s own relocation bootstrap, never by `CreateInstance` or
      anything in `HandleEvent` before the crash. This rules out "wrong
      ClsId" as the explanation and narrows the gap to `HandleEvent`'s
      own real control flow upstream of the crash site — not yet
      disassembled that deep; picking this back up should start from
      `HandleEvent`'s own entry (`0x0010b5b4`), not the crash site.
      **Followed through, and Super BurgerTime now reaches its real
      steady-state event loop.** Traced `HandleEvent` forward from its
      own entry: it's a thin real `AEEApplet`-template wrapper that
      calls the game's own handler (stored at applet+`0x18` by
      `CreateInstance`) before doing further built-in processing —
      and *that* built-in processing calls
      `ISHELL_CreateInstance(shell, ClsId=0x01001017, ppObj=&g_2e28fc)`,
      the same global this investigation already proved was otherwise
      untouched. `0x01001017` wasn't registered, so `CreateInstanceImpl`
      correctly failed and correctly left the output unwritten — but
      real code never checks the status and dereferences the still-null
      result two instructions later. **Not a new kind of gap after
      all** — the same "unchecked CreateInstance failure" pattern
      already solved for Peggle and Double Dragon. Fixed with the same
      generic scaffold treatment. **Verified**: `HandleEvent` now
      returns real success, and execution reaches "Reached the event
      loop with no unhandled instruction!" — the same milestone already
      hit for Double Dragon and Peggle, now on a third, independently-
      compiled title, stable over a 30-second run. No regression on
      either prior title; 250/250 tests pass.
      Overall this round: went from unable to execute a single real
      instruction past the common prologue to a third real commercial
      title reaching its steady-state event loop, across six
      independent, verified fixes (the ARM Extend instruction family,
      the stack/module address collision, three static-base slots, and
      this unidentified class). Super BurgerTime was substantially
      harder than Double Dragon/Peggle were at the same stage — a much
      larger `.mod`, a different still-uncracked asset container
      (`.pkg`), and a whole new bug category (the stack/module
      collision) neither prior title triggered.
      `resources.bar`/`.pkg`-style asset loading remains uncracked for
      this title (same as Peggle) — the natural next phase now that the
      steady-state milestone itself is reached. See PHASE8_LOG.md for
      full evidence.
      **Correction after starting on it**: cracking `.pkg` isn't
      actually the next concrete step. Found a real, substantial lead
      first — a string cluster (`"roms\neogeo"`, `"%s\%s.pkg"`,
      `"Loading romset %s"`, the real `"PACK"` magic literal) strongly
      suggesting this title embeds a generic, real, multi-system
      arcade-emulation core that loads ROM data via a conventional
      `.pkg` "romset" container, matching the project's earlier format
      survey — but no code in the compiled binary references these
      strings, and (more importantly) **nothing reaches file loading
      of any kind, packed or loose, and `ISHELL_SetTimer` is never
      called even once** across a 30-second driven run (both checked
      live, temporary instrumentation reverted) — unlike Double Dragon
      and Peggle, which both call `SetTimer` at least once. Reaching
      any asset load needs first understanding what drives this
      title's loop forward at all post-`HandleEvent`; given the
      arcade-core framing, it's also plausible the whole real game
      loop runs synchronously *inside* a single `HandleEvent` call
      (an old-style polling loop) rather than the event-driven
      `SetTimer` model the other two titles use — not yet
      distinguished. See PHASE8_LOG.md for full evidence.
      **Resolved**: `HandleEvent` is short (38 real HLE calls total,
      confirmed) and returns cleanly — not a synchronous full game
      loop. Its very last real call is on the class the previous round
      registered a generic scaffold for (`0x01001017`): slot 7 (byte
      offset `0x1c`) called with `(this, flag=0x4000,
      callback=0x11c06c, user_data=0)`. `0x11c06c` is real ARM code
      whose body is a textbook "process a list of registered objects,
      or return immediately if empty" shape — a real per-frame "run
      one engine tick" function, matching the arcade-core structure
      already suspected. The generic no-op scaffold silently discarded
      this registration, so the callback was never invoked. **Fixed**:
      added `IShellHle::ScheduleTimer` (refactored out of the existing
      `SetTimerImpl`) and scheduled this specific callback through the
      same real timer mechanism Double Dragon/Peggle's own loops use,
      on an *inferred* 16ms cadence (the real call site doesn't
      provide an explicit interval) — marked clearly as an inference,
      not confirmed real behavior. **Verified empirically**: tick 0
      now fires for the first time in this title's investigation, and
      real code runs inside the callback (real `MALLOC` calls) before
      hitting a new, deeper gap 95 steps in — direct confirmation the
      hypothesis was correct, not just plausible. No regression on
      Peggle/Double Dragon; 250/250 tests pass. Tracing the new gap
      inside `0x11c06c`'s own body is the next concrete step — a fresh
      thread, not yet started. See PHASE8_LOG.md for full evidence.
      **Continued into it**: two more static-base slots. Slot `0xd0`
      is called with `(name="boot", heap_object)` — `name` points at a
      real, in-module ROM manifest (`"boot\0boot.rom\0zupa_p1.rom\0
      zupa_s1.rom..."`; "zupapa" is this arcade original's real
      Japanese title) — confirming real romset-loading code is now
      actually executing. Fixing it (safe no-op) took the callback
      from 95 to 2,883 real steps, including a real pass through the
      `.pkg`/`"roms\neogeo"` string cluster the previous round found
      unreferenced — direct proof that code is live, not dead. Slot
      `0x184` is called `(flag=1, 0, table)`, too thin to identify;
      fixing it (same treatment) changes the failure mode entirely —
      real code no longer wanders, it settles into a genuine infinite
      polling loop (alternating between two real calls forever),
      almost certainly waiting on a real condition tied to the same
      ROM-manifest loading, that a static no-op stub can never satisfy.
      **This connects back to and validates the `.pkg`/romset question
      set aside two rounds ago** — real further progress now likely
      needs actually implementing romset loading, not another
      static-base slot. A substantially bigger undertaking than the
      incremental fixes so far; not attempted this round. No
      regression on Peggle/Double Dragon; 250/250 tests pass. See
      PHASE8_LOG.md for full evidence.
      **Traced the polling loop instead of assuming its cause**: root
      cause was a frozen emulated clock, not romset loading directly —
      `GetUpTimeMs` only ever advanced via the outer per-frame `Tick()`,
      which this loop's tight, single-native-call busy-wait never gave
      a chance to run. Fixed by having `GetUpTimeMsImpl` self-advance
      1ms on every read (still deterministic, inferred rate). Real
      code then breaks the first spin (~50 iterations, was infinite),
      makes 5 more genuine HLE calls (real `strcpy`, method calls on
      the SBT-specific class object), then hits a **second**,
      identically-shaped loop that does *not* resolve even with the
      clock advancing — confirms that one really is waiting on ROM-data
      readiness, not just elapsed time. Romset loading remains the
      next real undertaking. Also found and documented, while
      re-verifying no regression: this tool's `cls_id` argument must be
      each module's *real* embedded ClsId (found via trace, a literal
      compared against ClsId in `CreateInstance`'s first ~20
      instructions), not its download-folder number — the two only
      coincide for Super BurgerTime; Double Dragon/Peggle need
      `0x0102f789`/`0x01099cd6`, not `274754`/`278962`. 251/251 tests
      pass. See PHASE8_LOG.md for full evidence.
      **Traced the second loop precisely — the clock fix above works
      correctly** (confirmed the same bounded ~32ms wait function now
      resolves reliably, called thousands of times). The real remaining
      wall is one level up: the real per-frame callback (`0x11c06c`)
      walks a one-entry module-global "task list" (our SBT object) and
      calls three of its vtable slots every pass; every single code
      path loops back unconditionally, and nothing ever clears the
      list-head pointer to let it exit — that would need to be a real
      side effect of one of those slots' real implementation (almost
      certainly self-deregistering once the romset load it's presumably
      driving completes). Refines rather than replaces the
      romset-loading conclusion: implementing real, stateful behavior
      for class `0x01001017` (not just a generic scaffold) is the
      actual next step. Not attempted this round — diagnosis only, all
      trace instrumentation reverted, no functional changes. See
      PHASE8_LOG.md for full evidence.
      **Implemented the minimal fix and it worked**: a live memory
      watchpoint confirmed `0x2e28fc` (the same address `CreateInstance`
      writes the SBT object into) is real code's one and only exit
      condition, never cleared by anything reachable. Added a second
      override (slot 11 / byte offset `0x2c`, confirmed to be the real
      "tick" method) that clears it on the first call — an honest,
      minimal placeholder, not a claim about real loading time.
      **Super BurgerTime now reaches "Reached the event loop with no
      unhandled instruction!"**, matching Double Dragon/Peggle. Past
      it, hits a new, different gap ~1M real steps into the first tick:
      an `UnimplementedInstruction` at module offset `0x9c` where the
      *live* instruction word doesn't match the raw `.mod` file's
      content at the same address — a real return address whose target
      memory changed between call and return. Not yet understood; flagged
      as the next thread, deliberately not chased this round. No
      regression on Peggle/Double Dragon; 251/251 tests pass (fix is
      entirely in `tools/game_probe.cpp`, not core HLE). See
      PHASE8_LOG.md for full evidence.
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
