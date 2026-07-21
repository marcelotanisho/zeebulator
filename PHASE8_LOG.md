# Phase 8 Investigation Log — Double Dragon (and, from the log's final
# section onward, a second title: Peggle; a third, Super BurgerTime,
# starts near the very end)

This is the detailed, blow-by-blow investigation log for TASKS.md's Phase 8
task "Iteratively debug against the real game, filling HLE API gaps as
they're hit." It was split out of TASKS.md once it grew large enough to
dominate that file — see TASKS.md for the current summary/status and the
rest of the project's task list.

Every entry here is grounded in real evidence (real disassembly via
`arm-none-eabi-objdump`, real bundled SDK/header extraction, or live
runtime instrumentation) against the real, compiled game module —
never guessed. Entries are in chronological order (oldest first); the
most recent entry reflects current status. Almost all entries are
against Double Dragon's `ddragonz.mod`; the final section is against a
second real title, Peggle's `peggle.mod`, brought in to check whether
HLE work tuned against one game generalizes to another.

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

---

## Second real game: Peggle

Rather than keep pushing on Double Dragon's asset-level dead end, brought
in a second real commercial title to test something Double Dragon alone
never could: whether HLE work this deeply tuned against one game's real
code actually generalizes, or was accidentally overfit to it.

**Sourcing, and an unplanned but useful side-verification.** Downloaded
all 61 real games from the `zeebo-arquivista` archive.org preservation
item (https://archive.org/details/zeebo-arquivista, a curated ~700MB
Zeebo collection distinct from this repo's original Double Dragon
source) into `research/games/_archive_org_zeebo-arquivista/`
(git-ignored, same as every other real game asset in this repo). Before
picking a second title, re-downloaded Double Dragon from this
independent source specifically to test the open question from the
previous section: is this repo's `sound.ggz` possibly an incomplete
research-asset dump? Compared all 5 real files
(`274754.mif`/`data.ggz`/`ddragonz.mod`/`ddragonz.sig`/`sound.ggz`)
byte-for-byte (SHA-256) against the copy already in this repo: **all 5
are identical**, including `sound.ggz`. This rules out "incomplete dump"
as the explanation for the entry-74 EOF mismatch documented above — the
file is confirmed authentic and complete from two independent sources,
which narrows that open question rather than closing it (see that
section for the remaining hypotheses).

**Format survey across all 61 titles, an important and unplanned
finding**: only Double Dragon uses the GGZ archive format this repo's
loader (`core/loader/ggz.h`) already supports. Every other title uses a
different real container. The classic-arcade ports (`Bad Dudes vs.
DragonNinja`, `Caveman Ninja`, `Dark Seal`, `Heavy Barrel`, `Karnov's
Revenge`, `Street Hoop`, `Pac-Mania`, `Super BurgerTime`, etc.) ship
loose per-asset files (`.tex`/`.wav`/`.fnz`/`.tga`) alongside a
`"PACK"`-magic `.pkg` container (real magic bytes confirmed directly:
`50 41 43 4B` = `"PACK"`, followed by a real embedded zlib stream --
`78 da`, zlib's "best compression" header -- found inside one entry's
raw bytes) -- consistent with these being ports built on an embedded
classic-arcade-hardware emulation core bundling original ROM data,
architecturally very different from Double Dragon's native BREW app.
Several real PopCap titles (`Peggle`, `Bejeweled Twist`, `Zuma's
Revenge`) instead ship a single clean `resources.bar`/`resources.dat`
archive alongside `<game>.mod`/`.sig` -- much closer in shape to Double
Dragon's own layout, though the header bytes checked (`Peggle`'s
`resources.bar`) don't match any publicly documented PopCap BAR
signature, so the real format is still unidentified, not assumed.
**Picked Peggle**: cleanest layout of the non-GGZ titles, and its
`peggle.mod` (274,124 bytes) is noticeably smaller than
`ddragonz.mod` (462,748 bytes).

**Found Peggle's real `IModule::CreateInstance` ClsId via live
execution, not hand-decoding.** Peggle's `AEEMod_Load`/`AEEMod_New`
prologue (file offset `0`-`0x2740`-ish) follows the identical real
`AEEModGen.c`-template shape Double Dragon's does (same MALLOC-based
`AEEMod_New(dwSize=20, ...)` call through the confirmed static-base
slot `0x68`) -- expected, since both are BREW SDK-compiled ROPI
binaries -- but its `CreateInstance` (real address `0x2630`, found by
resolving the vtable-populating PC-relative literal stores in
`AEEMod_New`'s tail) is a *thunk* that jumps through a stored function
pointer (`module+12`) rather than doing an inline class-ID compare like
Double Dragon's. That pointer is null on the very first real
`AEEMod_Load` call (confirmed live: the two stack args `AEEMod_New`
forwards for it come from memory Double Dragon's equivalent call site
also zeroes), so real `CreateInstance` instead falls through to a
generic real dispatcher at module offset `0xcc8`. Rather than keep
hand-decoding this unfamiliar shape, just ran it live (`trace=true` on
the `CreateInstance` call, same technique used throughout this log) with
a guessed ClsId (the folder name, `278962` -- wrong, as expected) and
read the real comparison directly out of the trace: `cmp r4, r0` at
module offset `0xce8`, with `r0` loaded from a literal at `0xcd8` whose
real value, cross-checked directly against the raw file bytes at that
exact literal-pool address, is **`0x01099CD6`** (decimal `17407190`) --
unrelated to Double Dragon's own real ClsId, `0x0102F789`; different
game, different constant. Re-ran with the real value: `CreateInstance` reaches
real, substantial code (accesses `AEECLSID_DISPLAY` = `0x01001001`,
matching Double Dragon's own confirmed real value) but wanders outside
the module at step 33, jumping to `pc=0x00000000` -- the same "missing
static-base slot -> null function pointer -> jump to zero" shape
documented earlier in this log for Double Dragon's own early
`AEEMod_Load` gap.

**Found and fixed the real gap: static-base slot `0x9c`, a debug-logging
function.** Traced the indirect call at module offset `0xd18`
(`ldr ip,[r0,#0x9c]; ...; bx ip`, where `r0` is the confirmed
static-base table address) and its arguments by resolving every
PC-relative literal address by hand and reading the real bytes at each
directly out of the file (not guessed): the destination buffer passed
as the first call's first argument literally contains the ASCII bytes
`"*dbgprint"`; the source argument is a real Windows build-machine path,
`"e:\Peggl..."` (a debug build artifact). A second, differently-shaped
call through the *same* slot immediately after (module offset `0xd30`
onward) passes a real literal format string as its first argument --
found three at nearby call sites: `"CREATING APPLET: %x"`, `"FAILED TO
CREATE APPLET %x"`, `"FAILED TO ALLOCATE MEMORY %x"`. This is BREW's
real `DBGPRINTF` macro family (different fixed-arity real helper calls
per how many `%`-substitutions the format string needs, all funneling
through one static-base slot). Checked every real call site found (161
total, `grep`-counted) for whether the return value is ever used
afterward -- it never is, the very next instruction after every call
overwrites `r0` before it could be read -- so implemented as a pure
no-op (`core/brew/mod_runtime.{h,cpp}`, `kDbgPrintfSlotOffset = 0x9c`,
one new test `DbgPrintfSlotDoesNotCrash`). Committed (`b0788bc`).
**Verified against the real game**: re-ran Peggle's `CreateInstance` --
now completes in 141 real steps (versus wandering after 33 and running
786,680 steps of a real-but-untrustworthy excursion before), returns
real `SUCCESS` (`r0=0`), and writes a real non-null applet pointer
(`0x80300024`).

**Blocked on a new, different, and more architectural gap: Thumb-mode
code.** Driving `HandleEvent(EVT_APP_START)` on the real, non-null
applet immediately throws: a real `BX`/`BLX` at module offset `0xa4c`
targets an odd address, requesting Thumb (T32) instruction state.
`core/cpu/arm_interpreter.cpp` (~600 lines) is ARM-only -- it has no
Thumb decoder and no ARM/Thumb interworking at all, because Double
Dragon's `ddragonz.mod` apparently never needed one. This is
qualitatively different from every other gap in this log: those were
all missing HLE call-outs or table slots (bounded, mechanical,
find-the-real-behavior-and-wire-it-in work); this is the CPU
interpreter itself missing a whole instruction-set mode, which would
mean a second 16-bit decoder plus correct mode-switching semantics --
a real, separate feature, not attempted this round. Deliberately
stopped here rather than starting that work without discussing scope
first.

**Where this leaves the Peggle thread**: real ClsId found and verified;
one real, general (not Peggle-specific) HLE gap found, fixed, tested,
and committed, benefiting any future title that hits the same slot;
`CreateInstance` now succeeds cleanly end-to-end.

**Implemented Thumb (T16) decoding and ARM/Thumb interworking.**
`core/cpu/arm_interpreter.{h,cpp}` gained a full second decoder: all 19
real instruction format groups from the ARM Architecture Reference
Manual's Thumb instruction set summary, matched bit-for-bit against
that public specification (the same kind of real-evidence grounding
this whole log uses for BREW APIs, just against the CPU's own public
ISA spec instead of a game binary) -- except software interrupt
(format 17) and the two reserved condition codes in format 16
(`0b1110`/`0b1111`), both of which raise `UnimplementedInstruction`,
matching how ARM-state SWI is already handled. Also implemented real
interworking per the ARMv5T+ rule ARMv6/ARM1136J-S includes: `BX`/`BLX`
in both instruction states, Thumb's `POP{pc}`, and ARM's `LDR`/`LDM`
loading directly into `pc` all now select ARM or Thumb from the target
address's bit 0 (previously, ARM's `BX` to a Thumb target threw
outright, and ARM's `LDR`/`LDM` into `pc` never checked bit 0 at all --
both real gaps this closes, not just the one that blocked Peggle).
33 new tests (`tests/thumb_test.cpp`), one per format plus interworking
and two deliberately-chosen edge cases: the real, spec-defined "loaded
value wins over writeback" rule for `LDM` with the base register in its
own register list, and rejection of a real Thumb-2-only encoding
(`SXTH`, `0xB200`) that's genuinely unassigned in the classic Thumb1
ISA ARMv6 implements. Every encoding used in the tests was generated
independently via a small Python script from the same bit-layout
description cited in the implementation's own comments, then the two
were cross-checked against each other -- not hand-copied into both
places, to avoid the test oracle and the implementation sharing the
same mistake.
**Verified against the real game**: re-ran Peggle's `HandleEvent
(EVT_APP_START)` -- it now executes real Thumb code for 25,003 steps
before hitting the next real gap (previously: an immediate throw on the
very first Thumb `BX`). All 234 tests (33 new Thumb tests plus every
pre-existing test, none touched) pass.

**Found two more real static-base gaps this same round, using the newly
now-working Thumb decoder to trace further than was previously possible.**
Both found via the same method as every other slot in this log: let
real execution wander to `pc=0x00000000` (the standard "missing
static-base slot" signature), find the exact call site through the
confirmed table-fetch idiom (`ldr rX,[pc,#N]; add rX,pc,rX; ldr
rX,[rX,#-4]`), and read real evidence (arguments, literals) directly
out of the file's bytes.
1. **Offset `0x44`, a second MEMCPY-equivalent.** The one real call
   site unambiguously reachable through the confirmed table-fetch idiom
   (`peggle.mod` offset `0xc718`) calls it with exactly memcpy's
   calling convention -- `(dest=sp+28, src=sp+4, count=24)`, a plain
   24-byte stack-struct copy -- and its return value is read
   immediately after, matching `void *memcpy(dest,src,n)`'s "returns
   dest" contract too. (A second, coincidentally-same-numbered access
   at a different call site turned out to be an unrelated object's own
   vtable slot, not this table -- ruled out by checking for the
   table-fetch idiom specifically, not just the numeric offset.) Most
   likely a real ARM EABI helper symbol (e.g. `__aeabi_memcpy`) a
   compiler can emit separately from user-callable `memcpy` for
   compiler-generated struct copies, but behaviorally identical --
   wired to the same `MemcpyImpl`, one new test confirms the alias
   behaves identically to the original slot. Committed alongside the
   Thumb work.
   **Verified against the real game**: real execution now reaches
   25,833 steps before the next gap (up from 25,003).
2. **Found, not yet fixed: a third real field on the shared "app
   context" struct.** The confirmed offset-`0xc0` slot (documented
   earlier in this log as returning a struct with `IShell` at `+12` and
   `IDisplay` at `+20`) has a third real field, `+0x2c` (44), that real
   Peggle code reads and dereferences expecting an actual object: `r0 =
   context[0x2c]; r2 = *r0; r3 = *(r2+4); r2 = r3+r2; bx r2` (module
   offset `0xafb4`-`0xafc8`) -- a vtable-style call, though the final
   `r3+r2` addition is an unusual shape worth understanding properly
   (possibly a relative/position-independent vtable entry, not a plain
   absolute function pointer) before implementing anything. Our context
   struct only ever writes `+12`/`+20`, so `+0x2c` reads back `0`,
   `*0` also reads `0` (our `Memory` returns 0 for unmapped addresses),
   and the final `bx r2` (r2 still `0` after the chain) is what lands
   back on the familiar `pc=0x00000000` signature. Deliberately not
   guessed -- what real interface this field is meant to expose isn't
   yet known (candidates worth checking against the bundled real BREW
   SDK reference material: some other ambient/shell-adjacent interface
   `AEEApplet`-style context structs commonly carry) -- tracked as the
   concrete next step.

**Where this leaves things now**: the CPU interpreter itself is no
longer the blocker for Peggle -- everything remaining is back to the
same kind of HLE-gap, real-evidence-grounded work every other entry in
this log describes.

**Fixed the `+0x2c` context field with a general mechanism, not a
guess.** Re-examined the real call site (`ldr r0,[r9,#0x2c]; ldr
r2,[r0]; ldr r3,[r2,#4]; add r2,r3,r2; bx r2`, `peggle.mod` offset
`0xafb4`-`0xafc8`) and recognized the shape: the resolved call target
is `vtable_pointer + *(vtable_pointer + 4)`, not the ordinary `*(vtable
+ slot*4)` every other confirmed interface in this codebase uses. This
is ARM RVCT's documented "ROPI" (Read-Only Position-Independent) C++
virtual-function-table convention: entries store offsets *relative to
the vtable's own address* rather than absolute function pointers, so
the vtable itself never contains a load-address-dependent value.
Confirmed this isn't specific to one call site by checking the pattern
is a real, general ABI variant, not a one-off -- so rather than a
one-off workaround, added a proper general mechanism:
`scaffold_object.h/cpp`'s `BuildGenericRelativeVtableStubObject`
(mirrors `BuildGenericStubObject`'s exact role for absolute-vtable
interfaces, just storing `sentinel - vtable_address` at each slot so
the real ABI's relative-offset formula resolves back to the real
sentinel). Two new tests confirm the header and the real resolution
formula both work. `ModRuntime` gained a third settable context field,
`SetThirdContextObject()`, mirroring `SetShellInstance`/
`SetDisplayInstance` exactly -- the real interface's identity is still
unknown, so this is wired to the new relative-vtable-safe scaffold, the
same "observe, then replace with real behavior once identified" role
generic stubs play everywhere else in this codebase.

**That unblocked one more real gap: static-base offset `0x74` is
REALLOC.** Found by continuing the exact same trace once the `+0x2c`
call could resolve instead of wandering to zero. Two independent real
call sites (`peggle.mod` offsets `0x3b038` and `0x3b0dc`) -- two
separate growable-array template instantiations, one with 56-byte
elements, one with 4-byte elements -- both call it with `(old_ptr=the
array's current buffer, new_size=new_element_count * element_size)`
and check the result for non-null before overwriting their own buffer
pointer: exactly `void *realloc(void *ptr, size_t size)`'s real
contract, including "leave the old block alone on failure" (neither
call site's own logic would make sense otherwise). Implemented against
this allocator's existing no-free-list bump allocator (`ModRuntime::
Allocate()`, factored out of `MallocImpl` so both share it): allocates
a fresh block of `size` bytes and copies from the old block if both
succeed. Documented tradeoff, not a hidden gap: since this allocator
never tracks or reuses freed sizes, it copies `size` bytes from the old
block rather than `min(old_size, size)` (real realloc's contract) --
safe specifically because bump-allocated memory is never reused, so
anything read past the old block's real content is either still-zeroed
or not-yet-allocated memory, and the real callers observed always
immediately overwrite that tail with new elements right after a
successful grow anyway. Three new tests (grow-and-preserve,
fail-leaves-old-block-alone, null-pointer-behaves-like-malloc).

**A debugging detour worth recording, since it nearly got mis-
diagnosed as a bug**: with both fixes in place, re-running Peggle
appeared to hang after one single real trap call in tick 0 -- no more
output for 90+ seconds under a bounded `timeout`. Chased this
seriously (temporarily reduced the interpreter's own step budget,
separated stdout/stderr to rule out output buffering as the cause,
added raw fprintf/fflush instrumentation directly inside `Step()` and
`ExecuteBranchExchange`) before finding the real explanation: the
"hang" was a real `bx lr` where `lr` held exactly `0xf0000000` --
`trap_base`, the real sentinel address `HleRuntime`/`tools/game_probe.
cpp`'s own call-loop convention uses to mean "this ARM function call
completed, return control to the C++ caller." That's the *correct*,
successful completion of the tick-0 timer callback -- not a bug. Once
that returns cleanly, `tools/game_probe.cpp`'s own top-level loop does
exactly what it's designed to do for an interactive tool: delay 16ms,
tick again, forever, until a real `SDL_QUIT` (never sent in this
headless test run). All debug instrumentation was reverted (`git diff`
confirmed clean on `core/cpu/arm_interpreter.cpp` before committing).

**Verified against the real game**: Peggle's `HandleEvent(EVT_APP_
START)` now returns successfully (`1`), and the tool reaches "Reached
the event loop with no unhandled instruction! Window will stay open."
-- the exact same milestone Double Dragon reached, on a second real
game, using entirely general (not Peggle-specific) fixes. All 240 tests
pass.

**Next concrete steps**: (1) let Peggle run for many more real ticks to
see what real gap (if any) shows up next, the same iterative way every
title in this log has been debugged; (2) separately, reverse-engineer
`resources.bar`'s real format (still not started; Peggle's own header
doesn't match any known public format checked so far) -- needed before
Peggle can load its own real assets the way Double Dragon does.

---

**Ran (1) -- no new crash across thousands of real ticks.** Let the
tool run for 60+ real seconds (thousands of 16ms ticks) past the point
`HandleEvent(EVT_APP_START)` first succeeded. Confirmed indirectly
rather than by reading direct output (stdout is fully buffered when
redirected to a file, and the process is killed by an external
`timeout` rather than exiting -- so buffered output past the initial
setup lines is lost, the same trap the "false-alarm hang" entry above
already ran into): the process consistently required *external*
termination to stop. Every real error path in
`tools/game_probe.cpp`'s tick loop (`wandered_outside_module`,
`exceeded_step_budget`, a thrown `UnimplementedInstruction`) sets
`running = false` and lets the tool exit on its own -- so a process
that only stops when killed from outside, never on its own, is
consistent with (though not direct proof of) clean, uninterrupted
success across all of those ticks.

**Started (2), `resources.bar`'s real format -- made real progress on
*what* it is, not yet *how it's laid out on disk*.** Found the real
call site the same way as every static-base slot in this log: found
`"resources.bar"` as a real string thrice in `peggle.mod` (`0x3cdc8`,
`0x3d280`, `0x3ec64`), then wrote a small script scanning the whole
binary for the confirmed "PC-relative literal, add pc" idiom to find
which real code computes each string's address -- two real call sites
resolved cleanly (`peggle.mod` offsets `0x8ed0` and `0xa688`).
Disassembling the first's surrounding function (`0x8e90`-`0x8eec`)
shows it's **not a custom file-reading routine at all** -- it's a real
call through the confirmed `ISHELL` vtable (offset `0xc0`'s ambient
context struct, `+12` for the `IShell` pointer, then the real object's
own vtable at slot `0xa4`/41), with a five/six-argument shape: `(pIShell,
"resources.bar", id=r4&0xFFFF, type=0x5000, [sp]=0xFFFFFFFF, [sp+4]=
&local)`. Cross-referencing the real bundled `AEEShell.h` identifies
every piece of this precisely: `RESTYPE_BINARY` is literally defined as
`0x5000`; `AEE_RES_EXT` is literally defined as `".bar"` -- "Extension
of BREW Application Resources"; and the exact `(p,psz,id,t,b=-1,l)`
argument shape matches the real documented macro `ISHELL_GetResSize`,
`#define ISHELL_GetResSize(p,psz,id,t,l) (IShell_LoadResDataEx((p),
(psz),(id),(t),(void*)-1,(l)), *l)` -- the `-1` buffer sentinel is the
real, documented way to ask "just tell me the size, don't copy the
data." So: `resources.bar` is confirmed to be a **standard BREW
application resource file**, not anything Peggle-specific -- the exact
same real mechanism any BREW app's `.bar` resource file uses, just
happening to share a name with (and be otherwise unrelated to) PopCap's
own differently-shaped "BAR" archives on other platforms (ruled out
firmly this time, not just "doesn't match a public spec" as noted
earlier probing this file).
**Where this hits a real, structural limit the rest of this log hasn't
run into**: every other format/API this project has reverse-engineered
so far had its *implementation* inside a real, disassemblable `.mod` --
GGZ's reader, the static-base runtime-support table, every `IFile`/
`IShell` HLE call. `ISHELL_LoadResDataEx`'s real implementation lives in
the Zeebo device's own closed OS/firmware, not in `peggle.mod` at all
-- there is no compiled code in this repo's possession that parses the
real `.bar` binary layout. Cracking the format therefore needs a
different method than the rest of this log: blind, evidence-anchored
byte analysis of the raw file, verified against a known (resource ID,
real size) pair -- not disassembly of a caller. Tried to get that
anchor by tracing the real requested resource ID at the one real call
site found: neither of the two confirmed call sites (`0x8ed0`,
`0xa688`) is reached by `CreateInstance`, `HandleEvent(EVT_APP_START)`,
or the first several real ticks driven the same blind way as
everything else in this log -- meaning Peggle's real code only reaches
its own resource-loading path under some game state (a specific menu,
level, or asset category) not yet reached by driving ticks alone.
**Deliberately stopped here rather than guess the byte layout blind**:
without a real (ID, size) pair to check candidate structures against,
attempting to parse `resources.bar`'s directory now would be
undirected guessing -- exactly the kind of "wrong implementation risks
trading a clean, diagnosable failure for silent data corruption" this
log has avoided everywhere else. Tracked as the next concrete step,
either by finding what real game state reaches the resource-load call
(more disassembly of the surrounding real control flow), or by
resuming the raw byte analysis already started on the file's first 128
bytes (a plausible small header/count field around offset 0, then what
looks like a directory table with irregular strides starting around
offset `0x2c` -- not yet confirmed against any real value).

**Chased "what real game state reaches the resource-load call" and
found the real, structural reason: Peggle's main loop doesn't use the
self-rearming `SetTimer` pattern this codebase's whole tick-driving
model assumes.** Statically traced the real call chain backward from
the resource-load site (`peggle.mod` offset `0x8e90`) through three
real callers (`0x8860` -> `0x6d34` -> `0x2aca4`, the last flanked by
two more real `DBGPRINTF` calls with real source line numbers `216`/
`221`) to build a plausible reach path, then tested it directly with a
cheap PC watchpoint (temporary, reverted) on all three addresses across
60+ real seconds (thousands of ticks): zero hits. Also tried injecting
all 14 of the dev tool's mapped `AVK_*` key codes across many ticks in
case a menu/splash screen was waiting on input (temporary, reverted):
also zero hits, and critically, zero evidence any *further* tick ever
ran at all.
That last point led to the real answer: added a temporary live print
directly inside `IShellHle::SetTimerImpl` (reverted after use) and
confirmed `ISHELL_SetTimer` is called **exactly once** across the
entire run -- `(ms=20, callback=0x00132db0, user_data=0x80280200)`.
The callback address's bit 0 is clear (ruling out a real suspicion
this raised: that `tools/game_probe.cpp`'s `CallArmFunctionChecked`
dispatches a timer callback's raw pointer value as a PC transfer
without checking it for the real ARM/Thumb interworking convention a
function-pointer *value* is subject to, unlike a direct branch --
worth fixing generally if it's ever found to matter, but not the cause
here). And per this file's own earlier, already-fully-traced record of
tick 0's real execution (~24 instructions, one `GETAPPCONTEXT` call, a
clean `bx lr` return), that lone callback never calls `SetTimer` again
to re-arm itself.
Double Dragon's entire per-frame loop depends on exactly that real,
self-rearming pattern -- documented plainly in this project's own code
(`core/brew/ishell.h`'s class doc comment: "real game code re-arms its
own via `ISHELL_SetTimer` ... calling `SetTimer` again with the same
identity every time it fires"). **Peggle's real main loop evidently
does not work this way**, which is the real, now-confirmed reason
nothing past tick 0 -- including the resource-load call site -- is
ever reached by driving simulated time forward the way every title in
this log has been driven so far. What Peggle's real continuation
mechanism actually *is* (a different real BREW notification API, a
redraw/vsync-driven callback, or something else entirely) is not yet
identified -- this is now the concrete blocker for both making further
progress on Peggle at all and for finding a verifiable anchor to crack
`resources.bar`'s binary format.
All temporary instrumentation for this investigation (the PC
watchpoint, the key-injection probe, the `SetTimerImpl` print) was
reverted -- confirmed via a clean `git status`/`git diff` before moving
on. 240 tests still pass.

---

**Fixed the real timer re-arm gate.** Re-examined tick 0's callback
(`peggle.mod` offset `0x32db0`, already fully disassembled in the
entry above) closely enough this time to see it *does* try to re-arm
its own `ISHELL_SetTimer` -- gated entirely behind
`*(context[0x24] + 20)` being non-zero, a fourth real field on the
same shared "app context" struct as the confirmed Shell/Display/third-
object fields, which this codebase never wrote (so it always read as
null and the gate always failed). Unlike the other three fields, this
one is read and written as a plain data struct rather than through a
vtable -- the same callback stores a 64-bit timestamp at `+24`/`+28`
and reads another field at `+0x2a0`. Traced the time source
(`peggle.mod` offset `0x16cbc`) and confirmed it's nothing new: a thin
wrapper around this codebase's own already-working `GETUPTIMEMS` slot,
returning it zero-extended to 64 bits.
Added `ModRuntime::SetFourthContextObject`, wired in `tools/game_probe.
cpp` to a real, writable, zeroed memory block with only the one
confirmed-load-bearing field (`+20`) pre-set non-zero -- explicitly
framed (in both the code comment and here) as an educated, minimal
enabling stub, not a claim about what this struct actually is.
**Verified against the real game**: tick 0's callback now runs
hundreds of real HLE calls -- including a real `ISHELL_CreateInstance`
for class `0x01001003`, the exact same `FileMgr` class Double Dragon
uses -- where it previously made exactly one. Committed (`cf1b1fe`).

**Immediately hit a new, different gap, and traced it precisely: not
a missing HLE call this time, but a much bigger real structure than
our placeholder can safely stand in for.** Re-ran with full
instruction tracing bounded to tick 0 (temporary, reverted after) and
found the new wander-to-zero at real module offset `0x99f8`
(`peggle.mod` offset `0x9868`-`0x99f8`, a different real function from
the timer callback -- reached from it). The exact real sequence:
`bl 0x27594` (`GETAPPCONTEXT`, confirmed identical to every other real
call site) `-> ldr r0,[r0,#0x24]` (our new field) `-> add r0,r0,
#0x45000 -> ldr fp,[r0,#0x3d8]` -- i.e. real code treats
`context[0x24]` not as a small "manager" object with a couple of
meaningful fields, but as the **base address of a large global data
arena**, with different real subsystems reaching their own portion of
it through large, fixed offsets (`0x45000` here) from that same base.
Confirmed by adding `r11` to the trace print (temporary, reverted):
`fp` (`r11`) is null at the crash, exactly as expected, since our
placeholder object is only ~1KB and everything past it reads as
unmapped/zero by default -- not a bug in the fix itself, just
confirmation the real structure goes much further than what's been
implemented.
**Deliberately not extended further right now.** The `+20` "is ready"
flag was a single, narrow, well-evidenced field with a clear real
consequence (unlocking `SetTimer`); guessing at what real object
belongs at `context[0x24] + 0x45000 + 0x3d8` inside what is apparently
a large, multi-subsystem global arena -- with no idea yet how many
more such offsets exist elsewhere in it -- is a different, much larger
kind of guess, and exactly the risk this log has avoided everywhere
else ("a wrong implementation here risks trading a clean, diagnosable
failure for silent data corruption"). Tracked as the next concrete
step: either map out more of this arena's real layout via further
disassembly (now that the technique -- watch `GETAPPCONTEXT` ->
`context[0x24]` -> large fixed offset -- is established and
repeatable), or treat `context[0x24]` itself differently (e.g. as a
large real allocation the emulator provisions generously by default,
if further evidence suggests the arena's *contents* mostly don't need
to be meaningful except at a few specific, identifiable offsets like
this session's `+20`).
All temporary instrumentation (the `r11` trace column, the tick-0-only
trace flag) reverted -- clean `git diff` confirmed. 241 tests pass.

---

**Provisioned `context[0x24] + 0x45000 + 0x3d8` and immediately hit
two more real, evidence-grounded gaps in a row, both now fixed.**
First attempt wrote a generic stub object into that field during
initial setup, before any ARM code ran -- `fp` was still null at the
same crash site as before. A `Memory::Write8` watchpoint on that exact
address (temporary, reverted) caught the real cause: real
`CreateInstance`/`HandleEvent(EVT_APP_START)` code writes a real zero
to this same field once, as part of its own initialization (evidently
its own "not yet initialized" reset), unconditionally clobbering
whatever was already there. **Fix**: moved the write to happen right
before the "Reached the event loop" print, i.e. after `HandleEvent`
returns and after the real reset has already happened -- confirmed via
trace this unblocks the immediate null-pointer crash (step count moved
from 3100 to 3106).

Immediately hit a second gap one level deeper: the object's slot 2 is
called with a real QueryInterface-style shape (`this`, an id/flag, and
an output pointer `ppOut` in `r2`); our stub correctly returned success
in `r0`, but since it never touched memory, `*ppOut` stayed zero, and
the real caller dereferenced it without checking the status -- a new
null-pointer crash at step 3155. Patching slot 2 with a second stub
object fixed that one level, but the exact same shape recurred a third
time at the same step count, through the newly-returned object's own
slot 2. Rather than keep hand-patching one level at a time, generalized
to a recursive **self-propagating stub** (`std::function<uint32_t()>`
lambda capturing itself by reference, in `tools/game_probe.cpp`): every
slot of every generated object now lazily builds a fresh child object
of the same shape and writes it into whatever output pointer the real
caller passed, on demand, however deep a real chain of these turns out
to go. Explicitly marked EXPERIMENTAL and kept local to
`tools/game_probe.cpp` rather than promoted to
`core/brew/scaffold_object.h`, since this out-pointer-chaining shape is
not yet confirmed as a general real BREW ABI convention -- it's only
been observed at this one real call site so far.

**Verified against the real game**: tick 0 now runs vastly deeper --
real `ISHELL_CreateInstance(ClsId=0x01001003)` (the same `FileMgr`
class confirmed via Double Dragon), many real `Seek`-shaped
(`trap=0xf000029c`) calls, real `trap=0xf0000294` calls, and the
self-propagating chain itself visibly firing through real traps
`0xf0000584`, `0xf0000664`, `0xf0000700`, `0xf0000588` -- before
hitting a new, different-in-kind wall. First tried the self-propagating
stub with 10 slots per object and hit the exact same step-3155 wall;
tracing showed the failing read (`ldr r3,[r1,#0x30]`, offset 48 = slot
12) was simply past the end of a too-small 10-slot vtable, reading
unmapped memory as zero. Widened to 40 slots (matching this codebase's
established sizing convention for unidentified interfaces) and hit the
*same* step-3155 wall again -- this time confirmed via careful trace
re-reading that the instruction is not a vtable-indirected call at all:
it reads directly from `object+0x30` (the object's own memory), not
`*(*object+0x30)` the way every real vtable call handled so far works.
That is a third, genuinely different real object convention -- a flat
struct with a function pointer embedded at a fixed offset, distinct
from both the ROPI-relative-vtable shape (context's own third field)
and the standard absolute-vtable shape (every `BuildInterfaceObject`
call, including this arena object itself) already implemented.
**Deliberately left undoctored** -- the self-propagating stub's plain
zero-filled object memory reads as 0 there, producing the same clean,
diagnosable wander as every previous real gap in this log, rather than
guessing at what real function belongs at that offset. This is the
next concrete step for continuing this investigation.

Committed (`01fa50e`). All temporary instrumentation (the
`Memory::Write8` watchpoint in `core/memory/memory.cpp`, an `r11`
trace column in `core/cpu/arm_interpreter.cpp`) reverted -- confirmed
via clean `git diff` on both files. 241 tests pass.

---

**The "flat struct at offset 0x30" wall was a misdiagnosis -- the real
bug was in this codebase's own self-propagating stub, not a third real
object convention.** Re-enabled full register tracing for tick 0 only
(temporary, reverted after) and re-read the exact real call chain at
peggle.mod offsets `0x1099e0`-`0x109aac` closely, instruction by
instruction, rather than trusting the earlier quick read.

Real vtable slot 2 (offset 8) genuinely is the `(this, id@r1,
ppOut@r2)` QueryInterface shape the stub was built around -- confirmed
again at offset `0x1099e0`: `ldr r2,sp+0x28 (ppOut); ldr r3,[r0,#8]
(slot 2); ldr r1,[pc,#lit] (a real id constant, 0x0101eb0b); mov
r0,fp; bx r3`. But real slot 3 (offset 0xc) is a *different*, also
real shape: `(this, ppOut@r1)`, no id argument at all -- confirmed at
offset `0x109a98`: `ldr r0,[fp]; add r1,sp,#0x24; ldr r2,[r0,#0xc]
(slot 3); mov r0,fp; bx r2`. And real slot 4 (offset 0x10, confirmed
at offset `0x109a00`) is called with *no* output pointer whatsoever --
`r1`/`r2` just hold whatever leftover values earlier code left in
them. A further real call, `bl 0x105b50` at offset `0x109a94` (itself
a tiny two-instruction real trampoline, `ldr r3,[r0]; ldr r3,[r3,#0xc];
bx r3`, i.e. "call this->vtbl[3]" generically), is made with `r1=0,
r2=0` explicitly set by the real caller just before the call -- a real
"just checking, don't return anything" idiom.

The previous version of the self-propagating stub wrote a fresh child
object into r2 for *every* slot unconditionally, with no knowledge of
which shape a given real slot actually used. For slot 4's and the
`bl 0x105b50` call's real, no-output shape, this blindly clobbered
whatever r2 (or r1) held -- confirmed via re-tracing that one such
write landed on real address 0 itself (`r2` was legitimately 0 for
that call), silently seeding it with a stray child-object address. It
was *that* stray, self-inflicted value -- not a real "flat struct"
object -- that later got picked up, treated as a vtable pointer by
`ldr r1,[r0]` (r0 having read back 0 from an earlier never-actually-
delivered `ppOut`), and read at offset 0x30 (slot 12, well within the
real 40-slot vtable) as a function pointer, producing a bogus non-null
`bx` target and the misleading step-3155 crash.

**Fix**: only slots 2 and 3 get the self-propagating treatment now,
each using its own real, evidenced output register (r2 and r1
respectively), and each skipped entirely when that register is null
(so the real "just checking" calls are left alone rather than
corrupting address 0 or anywhere else). Every other slot (0, 1, and
4-39) is now a plain, side-effect-free stub -- consistent with this
codebase's normal `Stub` pattern elsewhere, and honest about what
isn't actually understood yet.

**Verified against real Peggle**: tick 0 now makes 337 real HLE calls
before its next wander (up from 207 before this fix), and survives
6312 real ARM steps (up from 3155) -- roughly double the real
execution depth in both measures. It still eventually wanders to
`pc=0` and throws the same downstream invalid-instruction exception at
module offset `0x90024` as before, but now from a **new, later, and
presumably genuinely different** real call site -- not yet
individually traced, since this round's fix was already a large,
self-contained, evidence-grounded unit of progress worth landing on
its own. Continuing to chase the new wander point the same way (full
register tracing, cross-referenced against real disassembly) is the
natural next step.

Committed (`6ab0d9f`). Rebuilt `minimal.ggz` (a synthetic, empty-but-
valid GGZ archive -- a real 8-byte one-entry table pointing at a real,
empty gzip member -- used as Peggle's `data.ggz`/`sound.ggz` stand-ins
since Peggle ships neither; the original copy from earlier sessions
lived in a since-expired scratch directory) to re-run the probe; not
committed, since `tools/game_probe.cpp` already documents how to
construct one and it's a throwaway harness input, not project source.
241 tests pass.

---

**Chased the new, later wander point from the previous fix -- and it
was a real gap this codebase already knows how to fill.** Re-enabled
full register tracing for tick 0 (temporary, reverted) and found the
new `pc=0` wander (at step 6312, up from 3155) came from real code at
`peggle.mod` offset `0x132dfc`: `ldr r0,[r4,#0x28]` (`r4` = the app
context address) with **no null check** before immediately calling a
small real subroutine at offset `0x131fac` with that value. That
subroutine (`ldr r1,[r4]; ldr r2,[r1]; add r2,r2,r1; bx r2`, after
first calling `GETAPPCONTEXT` to get `r0`/context again) is the exact
same ARM RVCT "ROPI" relative-vtable dispatch already confirmed and
implemented for the context struct's third field (`+0x2c`) -- i.e.
this is a **fifth confirmed field on the same shared app context
struct, offset `+0x28`**, using a convention this codebase already has
a scaffold for.

Checked whether this field needed the same write-timing care as the
fourth field's arena (a `Memory::Write8` watchpoint on
`context_address + 0x28`, temporary, reverted): real code never writes
it at all across the whole run -- purely read, never written, unlike
the arena's internal reset. And unlike the fourth field's arena
object, `GetAppContextImpl` already rewrites the *entire* context
struct fresh on every real `GETAPPCONTEXT` call (confirmed by reading
its own source, `core/brew/mod_runtime.cpp`), so there was no
write-timing hazard to work around here.

**Fix**: added `ModRuntime::SetFifthContextObject`, an exact mirror of
`SetThirdContextObject`, and wired it in `tools/game_probe.cpp` to
another `BuildGenericRelativeVtableStubObject` scaffold (its real
identity is just as unknown as the third field's).

**Verified against real Peggle -- this is the milestone the whole
Peggle investigation has been chasing**: the timer callback now runs
tick after tick with zero "wandered outside module" warnings and zero
thrown exceptions. Confirmed two ways: (1) a 15-second run with per-
tick HLE tracing enabled through tick 9 shows ten consecutive clean
ticks, each ending with the same real call sequence and no errors; (2)
an unbounded 60-second run needed external `timeout` termination
rather than exiting on its own -- the exact same "success looks like a
hang" signature already established and trusted for Double Dragon's
own steady-state event loop (every real error path in
`tools/game_probe.cpp`'s loops sets `running=false` and exits cleanly
on its own; nothing here does). Committed (`473758e`). 241 tests pass.

**Not yet done, and the natural next steps**: this is real *sustained*
execution, not necessarily real *correct* execution -- the third and
fifth context fields, the fourth field's arena beyond its one known
sub-offset, and every non-slot-2/3 method on the self-propagating
stub's generated objects are all still safe no-op placeholders, so
there's no guarantee real game logic (physics, scoring, resource
loading) is actually progressing rather than looping harmlessly. There
is also no visible output yet -- nothing has driven a real frame to
the SDL window. The next concrete step is watching (or tracing) what
the game actually *does* over many ticks: does real state visibly
change (level data loading, `resources.bar` finally being opened, a
frame rendered), or is it looping in place on placeholder objects that
never deliver real content forward.

---

**Answered that question directly: it's looping in place, not
progressing.** Added temporary debug prints (reverted after use, no
source changes remain) to every real "does something visible/external"
HLE call this codebase has -- `IDisplayHle::DrawText`/`DrawRect`/
`SetColor`/`Update`, `FileHle::OpenFileImpl`, and
`IShellHle::CreateInstanceImpl`'s unknown-class failure path -- then
ran real Peggle for 30 real seconds (roughly a thousand-plus real
ticks, going by the ~16-20ms real timer interval).

**Result**: only five real events fired in the *entire* 30-second run,
all during the one-time `CreateInstance`/`HandleEvent(EVT_APP_START)`
setup, none afterward: `OpenFile("udata/game", mode=1)` (read, fails),
`OpenFile("udata/game", mode=2)` (read/write, fails),
`OpenFile("udata/game", mode=4)` (create, presumably succeeds --
a real "load save data, create if missing" pattern), and two real
`CreateInstance` requests for classes this codebase doesn't implement
(`ClsId=0x0103d8ec`, `ClsId=0x01030766` -- real, evidenced leads for a
future round, not chased further this round). **Zero** `DrawText`/
`DrawRect`/`Update`/`SetColor` calls the entire run -- the game never
draws anything -- and **zero** further file opens once ticking began,
including no attempt to open `resources.bar` itself.

Cross-checked by diffing the full per-tick HLE call trace (all 20 real
calls, trap addresses and register arguments both) between tick 1 and
tick 5 of a separate traced run: **identical**, except for one
incidental pointer value differing by 16 bytes (not a growing
self-propagating-stub address -- those grow by 0x1000 per new object
-- so almost certainly stack/heap noise, not real state). Tick 1 and
tick 5 execute the exact same 20 HLE calls in the exact same order
with the exact same arguments.

**Conclusion**: the sustained, exception-free execution from the
previous two fixes is real, but it is a **fixed loop over placeholder
objects**, not real game logic progressing. The timer callback re-arms
itself and re-runs the same ~20-call sequence every tick indefinitely
-- consistent with the callback polling something (most likely one of
the still-unidentified interfaces behind the third/fifth context
fields' relative-vtable placeholders, or a sub-offset of the fourth
field's arena beyond the one confirmed `+20` gate) that our safe
no-op stubs can never report as "ready," so the real code that would
follow (loading `resources.bar`, drawing a frame) never runs. This is
the expected, honest limit of the "safe generic placeholder" approach
this whole investigation has used -- unblocking gates one confirmed
field at a time gets real code *running*, but getting it to do
something *meaningful* needs the placeholders' real identities, which
aren't known yet.

**Next concrete step for whoever continues this**: identify what the
third/fifth context fields' relative-vtable objects and the
self-propagating stub's non-2/3 slots are actually being asked for --
in particular, the literal ID constant baked into the module at the
confirmed slot-2 QueryInterface call (`0x0101eb0b`, `peggle.mod`
offset `0x1099f0`) is a real compile-time constant, and cross-
referencing it (and the two unknown `ClsId`s found this round) against
real BREW/Zeebo headers or other real `.mod` binaries already in
`research/` may reveal what real interface is actually being asked
for, the same way earlier class IDs in this project were identified.
All temporary debug prints reverted -- confirmed via clean `git diff`.
241 tests pass (no source changes to test).

---

**Chased the cross-referencing lead from the previous round.** This
repo's own reference BREW header subset
(`research/docs/sdk_installer_extract/brew_sdk_headers_reference/`) is
small (13 files) and had no exact match for any of the three IDs
(`0x0101eb0b`, `0x0103d8ec`, `0x01030766`), but it did establish the
real numeric convention: `AEEIID_IBase`/`AEEIID_IDisplay` are
`0x0103xxxx`, `AEECLSID_DISPLAY*`/`AEECLSID_DISPLAY_NULL` are
`0x0101xxxx` -- both unknown IDs sit squarely in those same real
families, consistent with being genuine platform-allocated IDs rather
than app-private ones.

More useful: a binary literal search (`struct.pack("<I", id)`) across
every real `.mod`/binary already in `research/games/` found
`0x0103d8ec` is **not Peggle-specific** -- it also appears in
`Super BurgerTime/mod/279125/supbtime.mod`. Disassembling both real
call sites (`peggle.mod` offset `0x104a50`-`0x104aa0`;
`supbtime.mod` offset `0x110e64`-`0x110ef4`) shows the **exact same**
real instruction sequence in both, independently-compiled titles: an
`AddRef`-shaped call on a real `IShell` pointer, then
`ISHELL_CreateInstance(pShell, 0x0103d8ec, &slot)`, and -- only if
that fails (`cmp r0,#0; beq ...`) -- a second attempt with a different
real ClsId (`peggle.mod` literal at offset `0x104ac8` = `0x01014bc4`;
confirmed identical at `supbtime.mod` offset `0x110f24`). Two
independently-compiled real games trying the exact same two literal
class IDs in the exact same fallback order is strong evidence this is
a real, standard SDK/compiler-emitted helper -- not anything
Peggle-specific -- even without a header match.

A third real ClsId, `0x01030766` (`peggle.mod` offset `0x10a208`-
`0x10a24c`), was found and traced separately: reached via a real
`IShell` pointer read from offset `+12` of the calling function's own
struct parameter (the same confirmed Shell-field layout as the ambient
app context struct), with `ISHELL_CreateInstance`'s result stored
**unconditionally** (no failure check at all) into that struct's own
offset `+0x48`.

**Fix**: registered all three with the same generic
`BuildGenericStubObject` scaffold already established for the
analogous `0x01002001` case (Double Dragon investigation, this same
log, above) -- deliberately not guessing at a real interface shape
neither header evidence nor call-site evidence actually supports yet.
Committed (`3045852`).

**Verified against real Peggle**: tick 0's total HLE call count
dropped from 340 to 331, consistent with the real fallback-
`CreateInstance` attempt now being skipped since the primary succeeds
-- confirming the fix took effect exactly as the real disassembly
predicts. **However**: the steady-state per-tick loop (tick 1 onward)
is completely unchanged -- still the exact same 20 real calls, same
order, verified by diffing the full trace before and after this fix.
**This rules out all three of these classes as the cause of the
per-tick progress plateau** documented in the previous round -- they're
one-time startup calls, not part of whatever the steady-state loop is
actually polling.

While tracing `0x01030766`'s call site, incidentally found a further,
adjacent real lead worth recording rather than chasing this round:
immediately after it (`peggle.mod` offset `0x10a254`), a separate real
function calls `GETAPPCONTEXT`, reads `context[0x24]` (the confirmed
arena base), and reads `arena+0x45000+0x3dc` -- four bytes past the
already-confirmed, already-provisioned `+0x3d8` sub-offset. This looks
like a sibling slot in the same small run of pointer-sized arena
fields, but reachability from the actual steady-state loop wasn't
confirmed this round (this function wasn't observed in any of the
traced ticks), so it wasn't provisioned -- a candidate for a future
round, not a confirmed blocker.

**Not yet resolved**: the actual per-tick blocker. The steady-state
loop's own real ID constant (`0x0101eb0b`, queried every tick via the
confirmed slot-2 QueryInterface shape on the fourth field's arena
sub-object) still has no header match and wasn't chased further this
round beyond the header/binary cross-reference above (also no match).
Identifying it likely needs either a fuller real BREW MP SDK header
set than this repo currently has, or continuing to trace structurally
-- what real methods get called on whatever object a real
implementation would eventually return -- the same way the context
struct's Shell/Display fields were originally identified.

---

**Web search for a fuller real BREW MP header set, or anything else
that might identify `0x0101eb0b`/`0x0103d8ec`/`0x01014bc4` --
exhausted for now, no matches found.** No search hit (general web,
BREW developer forums, GitHub) turns up any of these four hex values
in any indexed source. Two adjacent leads were checked and ruled out:

- **Infuse** (Tuxality's independent Zeebo/BREW HLE emulator, also
  written "from clean reverse engineering" per its own dev blog, and
  already reaching a playable steady state on Double Dragon, Crash
  Nitro Kart 3D, and Zeebo Family Pack) looked promising, but its
  source is **closed and proprietary** ("as-is", no redistribution, no
  modification) -- there is no code to safely reference even if it
  had solved this exact problem, and its public blog posts describe
  features only at a high level, with nothing about class IDs or the
  app context struct. It also doesn't target Peggle or Super
  BurgerTime, so it may never have hit these particular IDs anyway.
  Disassembling *its* compiled binary to extract a class-ID table
  would violate its own license terms and is a materially different,
  more invasive action than anything else in this project -- not
  attempted.
- **Actual Zeebo device firmware**: searched (not downloaded) for any
  public source of a real firmware/system image, both generally and in
  Portuguese. None found. This project's own already-sanctioned
  archive.org source (`zeebo-arquivista`) and the larger "Zeebo (All
  Games + Dev Tools)" archive.org collection both turn out to contain
  only games plus BREW SDK samples/PDFs already mirrored in this
  repo's `research/` -- no bootloader/OS-level dump in either. A
  GBAtemp thread referencing Zeebo console jailbreaking returned
  HTTP 403 and wasn't pursued further.

**Conclusion: this specific lead is exhausted.** No further internet
search is expected to help without a new, more specific starting
point. Identifying `0x0101eb0b` (or the other two now-registered-but-
unidentified classes) still needs either a fuller real BREW MP SDK
header dump than exists anywhere this search found, or continued
structural tracing of the real methods called on whatever object a
correct implementation would return -- the same evidence-only approach
used for every other interface in this project so far.

---

**Took the "continued structural tracing" option: full instruction
trace of one steady-state tick, to see what real code branches on.**
Re-enabled full per-instruction tracing for one representative tick
(temporary, reverted after). The very first thing the timer callback
does, every single tick, is real (`peggle.mod` offset `0x132de4`-
`0x132df4`): read `context[0x24]+0x45000+0x3dc` -- an immediate
sibling of the already-provisioned `+0x3d8` field, 4 bytes later,
confirmed reachable via two independent real functions now (this one,
and the `0x01030766` call site's own neighbor function from the
previous round) -- and pass it, completely un-null-checked, as `this`
into a real subroutine at offset `0x109088`.

That subroutine immediately dereferences its `this` parameter multiple
times: `ldr r0,[r0,#4]` (read), `str r0,[r5,#4]` (write back,
incremented -- a real per-tick call counter), `ldr r0,[r5,#12]` (read
a second field, presumably an array pointer), then `ldr r0,[r0]`
(dereference *that*). With the field left at 0 (this codebase's
default for anything unprovisioned), every one of those accesses reads
or writes **real, meaningful low memory addresses** -- confirmed via
the trace that real address 4 was getting a real incrementing counter
value written to it, every single tick, and real address 0 was being
read back through the chained double-dereference. This is real,
evidenced pollution of memory that has nothing to do with this field,
not simulated behavior of anything real -- exactly the kind of subtle
risk this log has flagged before ("a wrong implementation here risks
trading a clean, diagnosable failure for silent data corruption"),
except here the risk isn't hypothetical, it's directly observed.

**What this subroutine's real elements look like isn't understood well
enough to populate meaningfully**: offset `+4` is a real call counter,
`+12` looks like a pointer to a small (`<=4`-element, per a `cmp
r4,#4` loop bound) array, each element read at large offsets (`+0xbc`,
`+0xdc`) relative to a `4`-byte stride that doesn't obviously match a
`0xbc`+-byte struct -- consistent with this being **Peggle's own
internal per-tick game-object list** (particles, pegs, whatever),
anchored in the shared arena the same way the BREW-interface fields
are, rather than a generic BREW interface at all. Fully understanding
it would mean reverse-engineering Peggle's own gameplay data layout,
a materially bigger and more speculative undertaking than every other
field fixed in this investigation so far, all of which have been
identifiably generic BREW/ambient-context mechanisms.

**Fix, deliberately conservative**: rather than guess at that real
layout, gave this field the exact same treatment already used for the
fourth field's own arena allocation itself -- a real, writable, zeroed
memory block (`0x80050000`), just enough that the real accesses land
on memory that actually belongs to this field instead of colliding
with unrelated real addresses. This does **not** change what the real
subroutine does: a zeroed block still reads as "empty" at every offset
checked, so the same do-nothing branch is still taken every tick --
this is a hygiene/isolation fix, not a claimed progress unlock.

**Verified** via a full re-trace: `this` now resolves to `0x80050000`
instead of `0`, and the per-tick counter write correctly lands on
`0x80050004` instead of real address `4`. The double-dereference
(`ldr r0,[r5,#12]; ldr r0,[r0]`) still incidentally reads real address
`0` once, because this field's own `+12` slot is legitimately null (an
empty array, unknown real content) -- expected, harmless (the result
only feeds a `cmp`/`beq`, never a jump target), and not something
further guessing could safely improve. Committed (`47bfbf6`). 241
tests pass; a 30-second run against real Peggle remains stable (no
wander, no exception, needs external termination -- same steady-state
signature as before this fix).

**Overall state of the Peggle investigation**: the per-tick steady
state is now real, evidence-traced, and clean (no known memory
pollution), but it is still confirmed to be a fixed loop that never
progresses to real game logic (resource loading, rendering). The
concrete blocker is unchanged from two rounds ago: the loop's own real
ID constant (`0x0101eb0b`) has no identified real meaning anywhere
this project has looked (headers, other real `.mod` binaries, general
web search). Making further progress from here most likely requires
either a real BREW MP SDK header dump this project doesn't have access
to, or a much larger, Peggle-specific reverse-engineering effort into
its own per-tick game-object data (the `arena+0x45000+0x3dc` structure
found this round) -- both bigger asks than the incremental, evidence-
grounded fixes this log has made so far, and a reasonable point to
pause this specific investigation thread pending either new evidence
or a decision to invest in the larger effort.

---

## Third title: Super BurgerTime

Redirected effort here rather than continuing to push on Peggle's
specific per-tick-loop wall (see the pause point immediately above):
untapped, completely unprobed territory, and useful validation that the
HLE core generalizes rather than being overfit to two titles.

**Format survey (already recorded earlier in this log, see the format
survey entry above)**: Super BurgerTime is one of the classic-arcade
ports, shipping loose per-asset files (`.tex`/`.wav`/`.fnz`) alongside
a `"PACK"`-magic `.pkg` container — no GGZ, no `resources.bar`, a third
real asset-container shape distinct from both prior titles. Not solved
yet (see below); `supbtime.mod` (2,832,292 bytes) is reachable and disassemblable
the same way as the other two.

**`.pkg` container: magic confirmed, structure NOT a byte-exact Quake
PAK despite the coincidental "PACK" magic match.** Direct inspection:
`50 41 43 4B` ("PACK") followed by two little-endian `uint32`s at
offset 4/8. Naively read as Quake's classic 12-byte-header
`(dirofs, dirlen)` convention, the values are incoherent for a real
file (`dirofs=7`, which would point inside the header itself, and
`dirlen=579390` doesn't divide evenly by Quake PAK's 64-byte directory
entry stride). **Deliberately not assumed to be Quake PAK** just
because of the magic-byte coincidence — a real embedded zlib stream
(`78 da`) was already confirmed present elsewhere in the file (earlier
survey), so the real structure is still open; cracking it is deferred
until it's actually needed (asset loading hasn't been reached yet).

**Found and fixed a real, foundational CPU gap: the ARMv6 "Extend"
instruction family was entirely unimplemented.** The very first real
instruction Super BurgerTime executes beyond the common `AEEMod_New`
prologue (module offset `0xdc`) is `uxth r0, r0` — this interpreter's
`Step()` treated the *entire* ARMv6 media-instruction encoding space
(`bits[27:25]=011, bit4=1`) as unconditionally unimplemented, without
checking whether the specific real instruction inside that space was
actually understood. Confirmed the real encoding empirically (assembled
each real mnemonic with `arm-none-eabi-as`, read back the actual
bytes) rather than hand-deriving it from the ARM ARM's prose:
`bits[27:20]` selects `UXTB(0x6E)/UXTH(0x6F)/SXTB(0x6A)/SXTH(0x6B)`,
`bits[9:4]` must be `0b000111`, and `Rn=0b1111` selects the plain form
(any other `Rn` selects the real accumulate form, `Rd = Rn + extended
Rm`). Implemented all four plus their accumulate variants in one pass
— same real instruction family, and a real game hitting one is likely
to hit a sibling.

Caught a real correctness bug before it shipped, not after: naively
reusing `ShiftWithCarry` with `is_immediate_shift=true` for the
rotate-field-zero case would have reinterpreted `ROR #0` as `RRX` (a
real special case, but one that belongs to the *general shifter
operand*, not this family's own dedicated 2-bit rotate field) —
switched to `is_immediate_shift=false`, which correctly means true
"no rotation" at 0 while leaving nonzero rotate amounts unaffected.
9 new tests added (`cpu_test.cpp`), every encoding confirmed via the
assembler rather than asserted from memory. Committed (`ef1d3d4`).

**Verified against real Super BurgerTime**: `AEEMod_Load` now runs
742,000+ real steps past the point that used to throw immediately —
real, substantial execution, not a trivial unblock.

**Immediately hit a new, different, and much deeper real wall**: after
that long real run (confirmed to include a real BSS-zeroing loop, not
just wasted steps), a real function returns via the APCS
`push {fp,ip,lr,pc}` / `sub sp,fp,#12; ldm sp,{fp,sp,pc}` stack-frame
convention (loading the return address from the stack, not via `bx
lr`), and the popped return address is `0` — causing the exact same
"wander outside the module, execute harmless zero-page NOPs, then
coincidentally re-enter the module from its own start address" pattern
already documented for Double Dragon and Peggle's own early gaps. This
time, re-entry runs the *entire* AEEMod_New prologue a second time
(confirmed via trace: another ~2,000,000 real steps, including the
same real BSS-zeroing loop observed the first time) before a `bx r3`
lands on a real module address (`0x9c`) that decodes as an ARM `LDM`/
`STM` with the S-bit set (the exception-return/user-bank-register
variant) — a second real gap, `Block data transfer with S=1 ... not
supported`, though likely a downstream symptom of the same root cause
(the CPU executing real code at the wrong alignment/address after the
bad return) rather than a third, independent thing to fix.

**Not yet root-caused**: which specific call in the chain has a
stack-depth mismatch (an extra pop somewhere) or reads an
under-provisioned stack region as if it held a real saved return
address. This is a materially different *kind* of gap than anything
fixed for Double Dragon or Peggle so far — those were all missing
HLE calls or unprovisioned context fields; this is the CPU/stack
interaction itself going wrong deep inside the module's own real
compiled prologue, before any HLE surface is even reached. Tracked as
the next concrete step for continuing Super BurgerTime. `AEECLSID` for
`IModule::CreateInstance` also hasn't been found yet (blocked on
`AEEMod_Load` completing first) — deferred until this is resolved.

**Root-caused the null-return chain precisely, down to the exact
mechanism (temporary instrumentation throughout: an `arm_interpreter.h`
PC/register watchpoint, a `Memory::Write8` address watchpoint, and a
few debug globals bridging the two -- all reverted, confirmed via clean
`git diff`).** The bad `fp` (`0xffcffffb`) traced back one level
further than the previous round reached: `mov ip, sp` at real module
offset `0x10009c` -- the very instruction whose corruption we'd
already fixed once (it's the same address the original `uxth`
instruction lived at) -- was itself corrupted *again*, this time to
`0xe1e0c27d`, by the time it executed. Watching every write to that
address found the real culprit: a real ARM ROPI relocation-fixup loop
(module offset `0x100040`-`0x100054`):

```
100040: cmp   r3, r4
100044: ldrlt r6, [r3], #4      ; r6 = *table_entry (table walks r3->r4)
100048: ldrlt r7, [r6, r5]      ; r7 = *(r6 + base)  -- read the fixup site
10004c: addlt r7, r7, r5        ; r7 += base          -- apply the relocation
100050: strlt r7, [r6, r5]      ; *(r6 + base) = r7   -- write it back
100054: blt   0x100040
```

`r5` (the real relocation base for this pass) is computed just before
this, at offset `0x100018`-`0x10001c` (`sub r4, pc, #32` -- computes
the module's own real load address; `add r5, r4, #0x9c`) -- i.e. `r5 =
kBase + 0x9c` exactly, the address of the very `mov ip, sp`
instruction whose corruption started this whole investigation. That's
not a coincidence: this fixup pass legitimately treats its own load
address as the relocation base, since the table's entries (confirmed
directly against the raw file: 82,480 real, sane, strictly-increasing,
**never-zero** offsets, e.g. `0x110, 0x114, 0x118, ...`) are link-time-
relative offsets needing exactly this base added.

**The real table gets processed correctly once** (confirmed directly:
watching the first pass through `r3=0x2d9684` shows the real value
`0x110` read and a sane fixup applied) **-- then something writes zero
across the table's entire address range (`0x2d9684`-`0x329f44`,
confirmed as the exact range a separate real "clear it" loop at module
offset `0x100078`-`0x100084` walks) -- and then this exact fixup loop
runs a *second* time over the same, now-zeroed range.** On this second
pass, every table "entry" reads back as `0` (since the real data is
gone), so every iteration computes the exact same degenerate fixup
target: `r6 + r5 = 0 + r5 = r5 = 0x10009c` -- repeatedly reading,
adding `r5` to, and writing back that single address, corrupting real
code a little further on each of the loop's ~82,480 iterations (each
write is the previous corrupted value plus `r5` again, which is
exactly the "steadily climbing corrupted value" pattern observed
watching that address directly). This is what corrupts `mov ip, sp`
before it's ever executed, which is what puts the wrong value in `ip`,
which is what makes `sub fp, ip, #4` compute the garbage `fp` that
eventually gets restored from a stale stack slot and, several function
returns later, is read as a null "return to" address.

**Not yet resolved: *why* the fixup loop runs a second time at all.**
This is a real, structural question, not an interpreter bug in the
loop itself -- every instruction in it (including the conditional,
post-indexed `ldrlt`) was individually re-verified against a direct
memory read and behaves correctly; the *data* it's reading the second
time really is zero. Confirming whether this is genuinely intended
real behavior this codebase doesn't yet support (e.g. a legitimate
second relocation pass over reused scratch space, gated on something
this codebase hasn't provisioned) or a wrong loop-bounds/loop-count
computed earlier in the chain needs finding this loop's *caller* --
tracking the return address (`lr`) at both the first and second entry
to `0x100040` is the concrete next step, not yet done this round.

This is a categorically different, and by far the deepest, gap found
in this entire investigation across all three titles -- every previous
gap (Double Dragon, Peggle, and Super BurgerTime's own `uxth`) was
either a missing HLE call, an unprovisioned context field, or a
genuinely unimplemented CPU instruction; this is real, correctly-
emulated CPU execution reaching a real, self-inflicted data corruption
bug in the *module's own* real relocation logic, for a reason not yet
understood. A reasonable pause point given the depth already reached
this round -- the precise mechanism is now fully mapped for whoever
continues, down to the exact two loop addresses and the exact
corrupted value chain.
