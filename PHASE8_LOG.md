# Phase 8 Investigation Log — Double Dragon

This is the detailed, blow-by-blow investigation log for TASKS.md's Phase 8
task "Iteratively debug against the real game, filling HLE API gaps as
they're hit." It was split out of TASKS.md once it grew large enough to
dominate that file — see TASKS.md for the current summary/status and the
rest of the project's task list.

Every entry here is grounded in real evidence (real disassembly via
`arm-none-eabi-objdump`, real bundled SDK/header extraction, or live
runtime instrumentation) against the real, compiled `ddragonz.mod` —
never guessed. Entries are in chronological order (oldest first); the
most recent entry reflects current status.

---

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
**Follow-up research pass, no behavior changes**: went back
through two more bundled-but-previously-unopened real resources --
`research/docs/sdk_installer_extract/ZeeboSDKPackage-1.2.4/
samples.zip` (a *second*, larger real OGLES sample set beyond the
already-extracted `MSM7500_OGLES_...` one -- turned out to be the
same samples, but worth the check) and
`ZeeboDeveloperGuide0.97.pdf` (readable via `pdftotext`, not yet
read this session). Two payoffs, both corroborating rather than
contradicting earlier scaffolds (no code changes needed, comments
updated for the next reader):
- `simple_drawtexture.c` (a real bundled sample) does exactly
  `IDISPLAY_GetDeviceBitmap(...)` then
  `IBITMAP_QueryInterface(pIBitmapDDB, AEECLSID_DIB,
  (void**)&pDIB)`, then casts `pDIB` straight to
  `NativeWindowType` for `eglCreateWindowSurface` -- matching, in
  both call shape *and* downstream use, exactly what Double
  Dragon's own disassembly showed for the still-unidentified
  `0x01001045` scaffold. Strongly suggests `0x01001045 ==
  AEECLSID_DIB`, though (unlike `AEECLSID_GL`/`EGL`/`HID`) the
  numeric literal itself isn't in any bundled header, so this is
  circumstantial, not confirmed.
- The dev guide's own IHID walkthrough creates
  `AEECLSID_SignalCBFactory` immediately after `AEECLSID_HID`, to
  back IHID's connect/button-event `ISignal` callbacks -- the same
  order Double Dragon's disassembly showed for the still-
  unidentified `0x01041207`. Same caveat: matching order, not a
  confirmed literal. Also caught and fixed a wrong comment while
  cross-checking this: `0x01041207` is NOT gated on finding a real
  joystick (`GetConnectedDevices`'s own success/fail, not its
  device count, controls whether it's reached) -- it fires
  unconditionally in this run, and was already being scaffolded
  correctly; only the explanatory comment was wrong.
Also confirmed `IFont`/`ISprite` are real, Zeebo-supported
interfaces (per the dev guide's feature table) -- plausible
candidates for what `0x01001003`'s family is *for*, but with no
numeric ID or call-shape example anywhere in the bundled
materials, using that to implement slot 2's real behavior would
still be guessing, not verifying -- left as-is.
**The user asked directly: is this ninth gate actually necessary to
run the game? Answer, confirmed empirically: yes** --
`applet+0x24` must be exactly `0` (not `1`, `2`, or `3`) for the
per-tick game loop to run real logic instead of redrawing a
warning dialog every frame forever; this gate is the last of the
ten that was still nonzero.
**Found the real identity by reading the actual string/flags
involved, not just the call shape**: the "name" argument passed to
`0x01001003`'s slot 2 is a real, literal C string baked into the
file (`./udata/ddz.sav`, read directly from `ddragonz.mod` at file
offset `0x4e078`) -- a save-game path, not a font/texture name as
guessed last round. Full disassembly of the surrounding routine
(`0x237c4`/`0x9f3c`) shows a textbook "load save, or create a
fresh one" sequence: `IFILEMGR_Test` (slot 7) on that path, and on
failure `IFILEMGR_GetFreeSpace` (slot 8, checked against a
minimum) then `IFILEMGR_OpenFile` (slot 2) with a literal mode of
`2`, which is exactly real `AEEFile.h`'s `_OFM_READWRITE` (a
literal `4` appears too, matching `_OFM_CREATE`). That's
`IFileMgr`'s real vtable shape precisely -- so `0x01001003` is
very likely real `AEECLSID_FILEMGR` (still not a confirmed literal
number, same caveat as `AEECLSID_DIB`/`SignalCBFactory`, since no
bundled header states it numerically -- but the call shape and
flow leave little doubt).
**This project already had a complete, tested `FileHle`** (an
earlier phase, GGZ-backed, deliberately read-only) that had never
been wired into `CreateInstance` either -- same pattern as
`GlHle`. Wiring it in alone wasn't enough, though: the read-only
design would legitimately fail this exact "create a save file"
flow (`OpenFile` returning null for a path that doesn't exist in
the shipped GGZ content, `GetFreeSpace` returning `0`). **Extended
`FileHle` with a real, separate in-memory "user data" store**
(`core/brew/file_hle.h`'s `writable_files_`, distinct from the
read-only GGZ-backed `vfs_`): `OpenFile` now honors
`_OFM_CREATE`, creating a genuinely writable/readable file if
nothing else has it; `Write` (previously `StubFailed` for every
file) now really writes into a writable file's backing buffer
(growing it as needed) and still correctly fails for read-only
GGZ-backed files; `Test`/`GetInfo` recognize writable files too;
`GetFreeSpace` (previously a blind `Stub` returning literal `0`,
which would have failed this exact minimum-space check) now
reports a plausible simulated 1 MiB quota, optionally writing
total capacity through its real second `uint32*` argument. 8 new
tests in `tests/file_hle_test.cpp`: create-on-missing, write-then-
read-back round trip, `Test` sees newly-created files,
re-opening a created file by name sees previously-written bytes,
`GetFreeSpace` returns nonzero and correctly writes its optional
output pointer -- plus confirmed the existing
`ReadOnlyMethodsAllReturnAnError` test still passes unchanged
(GGZ-backed files remain genuinely read-only; only the new
writable store accepts writes).
**Verified against the real game -- the entire ten-gate chain
finally clears**: `applet+0x24` reads `0` after `CreateInstance`
for the first time this project. Real per-frame game logic now
runs -- tick traces show over a hundred distinct real HLE calls
per frame (texture/rendering-shaped, not the same six repeated
`DrawText` calls from the old warning dialog) across 4 full
ticks, a qualitative change from every previous run.
**Found a tenth gap this unblocked, of a new kind**: tick 4
throws `"Miscellaneous instruction space (MRS/MSR/etc.)"` --
not a missing HLE slot this time, but a real ARM instruction our
interpreter doesn't decode. Real cause (traced from the log, not
yet from fresh disassembly): a "wandered outside module" warning
(`pc=0x00000000`) fires a few real instructions earlier, meaning
something *else* -- most likely still one more missing/incomplete
HLE slot -- causes a jump through a null/garbage function
pointer; the CPU then walks forward through zeroed memory (which
happens to decode as harmless no-ops) until it reaches real data
at a low, non-code address that isn't a valid instruction. The
MRS/MSR error is a downstream symptom, not the real bug -- tracked
as the next concrete step: find what actually jumps to null in
tick 4 (a fresh instruction trace + objdump, same technique as
every fix this session, should pin it down directly).
**Found and fixed it**: an instruction trace of tick 4 (temporary
`trace=true` on that one tick, removed after use) pinned the null
jump to `ddragonz.mod` offset `0x23d0c` -- `ldr r3, [r0, #0x13c]`
off the static-base table, a **twelfth** table slot never mapped
(offsets `0x0`/`0x4`/`0x8`/`0x14`/`0x68`/`0x6c`/`0xb0`/`0xc0`/
`0xe4`/`0xe8` were already confirmed; this is `0x13c`). Reading
the actual arguments off the trace (not guessing): R1 wasn't a
plain integer as the calling convention alone might suggest -- it
was itself a real string pointer, and reading that string
directly out of the file gave `"ERROR CODE:%d"`. That, plus the
call being immediately followed by `STRLEN` on the destination
buffer, confirms a `sprintf`-family formatter -- unusual only in
that its third argument is `void **ppArgs` (a pointer to an
*advancing* args cursor, matching the double indirection at the
call site) rather than a plain `va_list`. Implemented as
`ModRuntime::SprintfImpl` supporting `%d`/`%u`/`%x`/`%X`/`%s`/`%c`/
`%%` (no width/precision/flags -- no evidence any real call needs
them yet). 3 new tests in `tests/mod_runtime_test.cpp`, including
one that reproduces the exact real confirmed case
(`"ERROR CODE:%d"` + arg `5` -> `"ERROR CODE:5"`, and confirms the
args cursor advances correctly).
**Verified against the real game**: the crash is gone -- the
previous ~15-second run (bounded by an external timeout, not a
crash) now runs cleanly through many more ticks with zero
exceptions, actually calling the new sprintf slot with the real
confirmed string and formatting a real error code into it
(screenshotted: the display now shows a different, several-line
message -- no longer the old "insufficient memory" dialog -- most
characters still render as the small unmapped-character fallback
box, consistent with the still-unresolved byte-swapped-string
quirk documented earlier in Phase 8, now applying to this new
string instead). Not yet investigated: *why* the game is
formatting an "ERROR CODE" message at all at this point (i.e.
what real error condition it detected, and whether it's a
legitimate real-hardware error path or something this emulator is
still getting wrong upstream) -- tracked as the next concrete
step, alongside decoding the message's actual text once the
byte-swapped-string handling is revisited.
**Follow-up investigation, no code changes**: added temporary
debug prints (removed after use) to `ModRuntime::SprintfImpl`
(dump the formatted string), `GlHle::EglSwapBuffers`, and
`IDisplayHle::Update` to answer two questions. First: what are the
actual values? `"ERROR CODE:6"` and `"LIST COUNT:3"`, redrawn
every tick (the same repeating-diagnostic pattern as the earlier
"insufficient memory" dialog, just a different message). Traced
the error-code field to a confirmed real struct offset
(`applet+0x36c4`, found via the real disassembly at
`ddragonz.mod` offset `0x10602c`) and found the *only* other real
reference to that exact offset in the whole binary is a reset-to-
zero at offset `0x1209dc` -- no direct `str` instruction
anywhere else constructs that same offset, meaning whatever sets
it to `6` does so indirectly (a computed/array-indexed write, or
a value copied in from elsewhere) rather than a single obvious
missing-HLE-call site like every fix so far this session. This is
real application business logic, not an emulator gap -- a
meaningfully different (and likely much longer) kind of
investigation than closing HLE/runtime-table gaps, so deliberately
not pursued further this round. Second: confirmed `IDisplayHle::
Update` fires every tick (148 times in one ~8s run) while
`GlHle::EglSwapBuffers` never fires at all (0 times) -- ruling out
a suspected rendering-pipeline conflict between the software
`IDisplay` framebuffer and the real GL context sharing one SDL
window (`Sdl2GlBackend::SwapBuffers` really does call
`SDL_GL_SwapWindow`, confirmed by reading
`frontends/standalone/sdl2_gl_backend.cpp`) -- moot for now since
the game hasn't reached real per-frame GL presentation yet, only
this diagnostic-overlay loop.

**Found and fixed a real gap this round, via a live memory watchpoint
on `applet+0x36c4`** (temporary, `Memory::Write32` -- confirmed the
field is legitimately written `0` a few times during setup, then a
single real write of `6` correlates exactly with one specific HLE call
sequence right before it). That sequence: `IFILEMGR_OpenFile(pFileMgr,
"sound.ggz", mode=1)` -- the game opens its own packed resource
archive **as a raw file**, by its plain filename, rather than expecting
every entry pre-extracted. `tools/game_probe.cpp`'s `MergeGgzInto` only
ever registered each GGZ archive's *decompressed entries* in the
`VirtualFilesystem`, never the archive's own raw bytes under its own
basename -- so `OpenFile("sound.ggz")` correctly, honestly returned
"not found." Fixed by also registering each archive's raw bytes under
`BaseName(path)` when loading (`vfs.AddFile(BaseName(path), raw)`).
**Verified against the real game** (a temporary debug print in
`FileHle::OpenFileImpl`/`SeekImpl`/`ReadImpl`, removed after use):
`OpenFile("sound.ggz", mode=1)` now succeeds (`file_size=1928097`,
exactly the real file's size), and the game goes on to genuinely
`Read`/`Seek` within it -- real audio-archive access now works, not
just a non-crash.
**The `ERROR CODE:6` loop itself persists past this fix, for a
different, deeper reason.** Traced the caller (a real loop at
`ddragonz.mod` offset `0x1c964`, disassembled directly): it iterates
a resource list (up to 81 slots) calling a helper at offset `0x1bfd0`
for each with `(pIShell, "sound.ggz", nameTable[i], &resultTable[i])`,
stopping as soon as that helper returns `0`. A second real open of
`"sound.ggz"` in this loop does `Open` -> `Seek(SEEK_START, 488)` ->
`Release`, with **no `Read` in between** this time (unlike the first,
successful open earlier in the same tick, which did `Read` before
seeking further) -- and the error gets set immediately after. This is
genuine resource-list business logic (why does entry N behave
differently from entry 0, what `0x1bfd0` actually validates, what
"`LIST COUNT:3`" really counts) rather than a missing HLE primitive --
would need substantially more disassembly of `0x1bfd0` and the
81-slot-iteration loop specifically to resolve, tracked as the next
concrete step.

**Found and fixed a real, foundational bug this round: `IFILE_Seek`'s
return value was backwards.** The real `AEEFile.h` documents `Seek`'s
contract precisely: it returns `AEE_SUCCESS`(0)/`AEE_EFAILED`(1), *not*
the resulting position -- the one documented exception being
`_SEEK_CURRENT` with `moveDistance=0`, a "tell" operation that does
return the current position. `FileHle::SeekImpl` had always returned
the resulting position instead, which only coincidentally matched real
"0=success" for the specific case of seeking to absolute position 0 --
so every previous test (and every previous real gate this session) that
happened to seek to position 0 looked correct, while any real seek to a
*nonzero* position returned a nonzero value real game code reads as
failure. **Found via the resource-list loop from the previous entry**:
its helper (`ddragonz.mod` offset `0x739c`) does
`ISHELL`-style `Seek(SEEK_START, index*8)` then checks the result
against 0 -- exactly the shape that would silently break for every
`index > 0`. Rewrote `SeekImpl` to match the real contract exactly,
including the real documented asymmetry between read-only files
(seeking outside `[0, size]` fails) and writable files (seeking past
EOF extends the file; only seeking negative fails) and the `_SEEK_
CURRENT`+0 tell exception. Rewrote the existing Seek test to check the
real contract instead of the old (wrong) one, and added 3 more:
tell-returns-position, out-of-bounds-fails-on-read-only, and
past-EOF-extends-a-writable-file.
**Verified against the real game** (a temporary live memory watchpoint
+ full instruction trace of the exact failing call, both removed after
use): `0x739c`'s *two* real `Seek` calls (one to the GGZ header-table
entry, one to the resource's real data offset) now both succeed
correctly, and the whole function completes and returns success for a
real resource index (61) that previously failed outright. **The
`ERROR CODE:6` loop still doesn't fully clear**, for a new, different,
narrower reason found by continuing the same trace one level deeper:
after the two real `Seek`s succeed, the resource-loading routine
(`0x1bfd0`) reads the file's real 8-byte GGZ header entry correctly
(offset `0x1c3e25`, size `0x9ef0` -- both parsed correctly), then tries
to `Read` the actual resource content through a *different* object,
`manager->12` (`manager` here is a distinct per-subsystem struct at
`applet+0x19c`, not the `applet+0x128` GL-init struct from earlier) --
and `manager->12` still points at the old, still-unidentified generic
scaffold for class `0x01001014` (the twin of `0x01001003`=`FileMgr`
found together at the very first `0x1b2fc` gate this session), whose
blind `Stub` slot 3 returns 0 bytes read every time. Nothing in
`0x739c`/`0x1bfd0` ever *writes* `manager->12` -- it must be
initialized once elsewhere, in a dedicated init routine for this
`applet+0x19c` subsystem not yet located. Tracked as the next concrete
step: find that init routine (the same technique as every fix this
session -- find what constructs `applet+0x19c`, trace its own
`ISHELL_CreateInstance` calls) to finally identify class `0x01001014`
for real, the same way `0x01001003` was identified.

**Follow-up investigation into the `0x01001014` object, no code changes
this round.** The previous entry's guess at *which* struct field was
the culprit was imprecise -- corrected here with direct trace evidence.
A live watchpoint on `applet+0x19c`'s own `+12` field found it gets
written `0x8030014c` (i.e. `applet+0x128`'s address), but that field
turned out to be an unrelated cached back-reference, not the actual
object dereferenced by the failing `Read` call. Re-reading the exact
instruction trace of the failing call directly (register values, not
re-derived by hand) showed the real culprit precisely: inside
`0x1bfd0`, its own first argument (confirmed via the trace to be
`applet+0x128`, not `applet+0x19c` -- `0x1c964` passes
`*applet+0x19c` i.e. `applet+0x19c`'s *own* `+0` field, which caches a
pointer back to the shared `applet+0x128` loader struct) is
dereferenced at `+12` to get the object, whose vtable slot 3 (`Read`)
is called. That's exactly `applet[0x128+12]` -- the real, confirmed
`0x01001014` object from the very first `0x1b2fc` gate -- still our
blind 40-slot generic scaffold, whose slot 3 just returns 0.
**Searched further for the class's real identity, still inconclusive.**
Re-examined `0x1b2fc` (where the object is created) directly: no
initialization call is made on it after `CreateInstance` succeeds --
it's created once and presumably used standalone later, with no
"attach me to a specific open file" step visible anywhere in the
traced call chain (`0x1b2fc` create -> `0x1c964` resource-list loop ->
`0x1bfd0` per-item loader -> `0x739c` open/seek/read-header, none of
which ever write `applet[0x128+12]` after creation). Chased one
promising real lead to a dead end: the same qx_cab SDK extraction that
named `AEEAppGen.c`/`AEEModGen.c` also contains real Qualcomm "QX
Engine" middleware headers, including `QXPackFileManager.h` and
`QXPack.h` -- a real pack-file archive reader many Zeebo-era BREW games
built on. Read both directly: `QXPack`'s own file format (string
table, directory records, `QXPackFile{fileNameStringID, fileSize,
fileDataOffset}`) does not match our own GGZ format's simpler
"N 8-byte (offset,size) entries" structure at all, and `QXPackFileMgr`
is a plain host-side C API (`QXPackFileMgr_Create(QXState*)`), not
something obtained via `ISHELL_CreateInstance` -- ruled out with real
evidence, not assumed. No further real leads found in this repo's
bundled materials for what `0x01001014` actually is. Deferred rather
than guessed -- a wrong implementation here risks trading "reads 0
bytes, harmlessly" for "reads garbage bytes, corrupts real game state
in a way that's much harder to notice or diagnose than the current
clean failure."

**Pushed past the deferral above and implemented class `0x01001014`'s
real behavior.** Every piece of real evidence gathered so far pointed
at the same shape: the object is created once, never explicitly bound
to a file, and its `Read` is always called immediately after the
game's own loader opens/seeks the file it actually wants through a
*separate* `IFileMgr` object. Modeled that directly:
`FileHle::BuildLastOpenedFileProxy` returns an object whose `Read`
(slot 3) forwards to whichever file `OpenFileImpl` most recently
returned, re-resolved on every call rather than fixed at construction.
Documented in both `file_hle.h` and the registration site in
`game_probe.cpp` as a deliberate, evidence-grounded implementation --
not a confirmed-correct one -- since no real header or SDK sample
found in this repo's bundled materials names the class. **Verified
against the real game** (live trace): items that previously read 0
bytes through the old blind scaffold now read real byte data.
**Also found and fixed a second, independent real bug in the same
pass: the emulated heap was exhausted partway through the resource
list.** `ModRuntime`'s heap was sized at 1MB arbitrarily, never
measured against real needs. The resource loader `MALLOC`s a real,
sizeable audio buffer (tens of KB to just over 1KB, varies per item)
for every one of the real list's entries in a tight loop; 1MB runs out
partway through and MALLOC legitimately returns real NULL, which real
game code has no path to recover from. Bumped to 16MB in
`game_probe.cpp` (a generous but not unreasonable amount of app heap
for a 2009-era dedicated gaming device) -- verified via live trace
that MALLOC no longer fails across a full run. Both fixes committed
together (`cb52c00`), with 3 new proxy tests
(`LastOpenedFileProxy*` in `tests/file_hle_test.cpp`) alongside the
existing Seek-contract tests.

**With both fixes in place, the loader gets *much* further -- dozens
of real resources load successfully -- but `ERROR CODE:6`/`LIST
COUNT:3` still eventually appears, now after processing far more
items than before.** Re-traced tick 3 in full (instruction trace grew
from ~3,300 lines pre-fix to ~13,500 lines post-fix) and followed the
new failure back to its exact cause. First, a side discovery that
corrects an earlier assumption in this log: `"LIST COUNT:3"` is *not*
a count of which resource-list item failed -- it's the per-tick state
machine's own step counter (the 12-case jump table at `0x1c1ec`
documented earlier), incrementing once per real tick regardless of
which resource failed or why. The dispatcher reports one hardcoded
generic failure code for the whole case (`case 2`), so this diagnostic
was never going to distinguish "item 3 failed" from "item 63 failed"
in the first place -- a dead end for narrowing the search that way,
reconciled now rather than chased further.
Second, and more useful: counted exactly 63 successful
`MALLOC`+`Read` cycles in the tick-3 trace before the new failure, and
found the specific real `Seek` immediately preceding it targets header
offset `584` -- i.e. `73 * 8`, the **74th and last** entry in
`sound.ggz`'s real 74-entry GGZ table (the file's own "loaded 74
entries" banner, printed at every run's startup, was hiding in plain
sight the whole time). That entry's real header bytes (read directly
out of the real file, confirmed independently with a small Python
script rather than trusted from the trace alone) declare
`offset=1927592, size=1034`; the real file is exactly `1928097` bytes
long, leaving only `505` bytes after that offset -- **529 bytes short
of the declared 1034**. Decompressing those exact 505 real bytes as
gzip (`zlib.decompressobj(16+zlib.MAX_WBITS)`) succeeds cleanly,
produces exactly 1034 bytes of output, and consumes the stream with
zero leftover bytes and `eof=True` -- i.e. this is a **complete,
valid, correctly-terminated gzip stream**, not truncation or a parsing
bug on our end. `size` in the GGZ header is the *decompressed* length
(matching this repo's own `ggz.h` documentation), but the real game
code at `0x1bfd0`/`0x739c` is reading `size` **raw, undecompressed
bytes directly off disk** in a loop that keeps requesting more
whenever a read comes up short of the running total -- which happens
to work for every other entry only because there's always more file
data *after* it (the next entries' compressed streams) to keep
pulling from until the requested total is reached, entry boundaries be
damned. The very last entry has nothing after it, so the read
genuinely, correctly comes up short (`505` then `0`), and `0x1bfd0`'s
own final `expected == actual` check (`1034 != 505`) fails -- exactly
matching the observed trace (`ldr r0,[r4,#0x14]; cmp r0,r5` at
`0x1c0a0`/`0x1c0a4`, branch taken to the fail path).
**Also confirmed this is a genuinely persistent failure, not
transient**: reran with `hle_trace` (no full instruction trace) across
10 real ticks and saw `"LIST COUNT:3"`/`"ERROR CODE:6"` printed
identically, unchanged, every single tick -- ruling out an earlier
open question about whether the state machine might recover on a
later tick.
**Not yet resolved**: whether real hardware's original `sound.ggz`
genuinely has more trailing data after this last entry (making our
copy of the asset the actual gap, not the emulator), or whether real
game code is expected to legitimately fail exactly this way for the
list's last slot and recover through a path not yet traced (a retry,
a skip, or a "give up gracefully and continue to gameplay anyway" that
our own state-machine driving hasn't reached or doesn't implement).
Tracked as the next concrete step -- continuing the investigation
rather than switching to something else, per direction.
Side note for future invocations: this round's investigation began
with a wrong assumption that the `.mod`'s containing folder name
(`274754`) was Double Dragon's real `IModule::CreateInstance` `ClsId`
-- it isn't (that was already corrected earlier in this very log, see
the `0x0102F789`/`16971657` entry above); re-confirmed directly
against the real compiled literal at file offset `0x738` before
re-running. `tools/game_probe.cpp` always takes the real ClsId as its
4th CLI argument, `16971657`, not the folder name.

**Resolved the "not yet resolved" question above, via real disassembly
of the dispatcher itself (`0x1c170` onward, the true function entry --
`0x1c1ec`/`0x1c964` from earlier entries are both part of the same
function) plus a live watchpoint, no guessing.** Two findings, one of
which *corrects* an earlier entry in this log:
1. **Correction**: `"LIST COUNT"` and the per-tick case-dispatch index
   are not two related-but-distinct counters as the previous entry
   concluded -- they are the exact same 16-bit struct field
   (`applet+0x36b2`). The dispatcher reads it as `r1` at entry
   (`ldrsh r1,[r4,#2]` at `0x1c18c`) to pick which of the 12 cases to
   run, then unconditionally increments and stores it back
   (`0x1c3c8`-`0x1c3d0`) before returning. So every real tick this
   dispatcher runs, it advances to the *next* case, permanently -- this
   is a run-once-through-12-states sequencer, not a retry loop.
2. **The real reason the diagnostic never changes turns out to be much
   simpler than "stuck retrying case 2 forever"**: it isn't retried at
   all. A live watchpoint on both fields (`applet+0x36c4`
   error-code, `applet+0x36b2` count) across a full ~900-tick run
   showed exactly four writes total -- count going `0 -> 1 -> 2`, then
   error-code `-> 6` and count `-> 3`, all within the first 3 real
   ticks -- and **zero further writes to either field for the
   remaining ~900 ticks**. The whole dispatcher function opens with an
   unrelated early-out guard (`0x1c170`: if `[r1+4] == 0`, write `1`
   and return immediately, touching neither field) -- consistent with
   whatever higher-level code owns this subsystem simply no longer
   calling this stepper at all once loading has failed, rather than
   calling it and hitting a guard every time (a guard trip would have
   shown up as repeated writes of `1`, which never appeared). The
   values displayed every frame afterward are just whatever was last
   left in memory from tick 3, redrawn by a separate, unrelated render
   path -- not evidence of an active retry.
   Also confirmed independently via static disassembly of the real
   call site (`0x1c250`: `mov r3, #81`) that the loop's 81-item bound
   is a **hardcoded literal**, not derived from any real parsed count
   -- so the real game genuinely, unconditionally attempts up to 81
   sound-resource slots every time this state runs, regardless of how
   many entries `sound.ggz` actually has. Combined with the previous
   entry's finding (failure lands exactly on real entry 74 of 74, via
   genuine end-of-file exhaustion, not an out-of-range index), this
   makes it likely that our copy of `sound.ggz` is missing trailing
   data the real distributed file has -- i.e. **the current best
   hypothesis is a research-asset gap, not an emulator gap**: if the
   real file had enough trailing bytes for entry 74's raw read to
   reach its full declared 1034 bytes (by spilling into further, even
   if unused, archive data the same way every earlier entry does),
   this exact failure would not occur. No further real evidence in
   this repo's bundled materials to confirm or rule that out -- no
   code changes this round, purely investigative.
3. Also fully, statically disassembled `0x1bfd0`'s tail (`0x1c09c`-
   `0x1c0f8`) to rule out a missed bypass: the final
   `cmp [manager+20](expected), r5(actual)` is a strict equality gate
   with no partial-acceptance path -- on any mismatch it unconditionally
   falls through to a cleanup branch that returns real `0`. Confirms
   there is no alternate real code path that would let a short read
   like this one succeed; the only way past this specific gate is more
   real bytes at that file offset.
4. Tried one more thing before concluding this thread: temporarily
   injected all 14 of this dev tool's currently-mapped `AVK_*` key codes
   (`SdlKeyToAvk`'s number-key and arrow-key range) as real
   `HandleEvent` key-down/up pairs spaced across ~450 further ticks,
   on the chance the frozen subsystem is actually waiting on player
   input (an "insert coin"/"press start" style dismiss) rather than
   being permanently dead. A live watchpoint across the whole run showed
   zero further writes to either field for any of them. Inconclusive
   rather than a firm no, since this tool's key mapping is explicitly a
   guess, not a confirmed real one (see `SdlKeyToAvk`'s own doc comment)
   -- but no evidence found that input is the missing piece either.
   Reverted (no code changes kept from this experiment).

**Where this leaves Phase 8**: every concrete, findable-with-real-
evidence gap in the emulator's own HLE has been closed for this code
path. What remains blocking real playability is a single real resource
(`sound.ggz` entry 74 of 74) that this repo's copy of the file cannot
satisfy a strict, exact-match real read against, for a reason (genuine
end-of-file) that's a property of the *asset*, not of anything the
emulator does. Making further progress here most likely needs a
different real copy of `sound.ggz` (or of the whole game package) to
compare against, rather than more disassembly of code already read
exhaustively down to its last real branch.
