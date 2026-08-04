# Zeebulator — Dev/QA Tooling Task Breakdown

Companion to `TASKS.md` (emulation-correctness work) — this file tracks
player-facing QA/dev-experience tooling for `tools/game_probe.cpp` (or its
eventual promoted successor): resolution scaling, save states, and
controller binding, plus hotkeys and an on-screen overlay to control them
live. Exists so
bug-hunting sessions (see `PHASE8_LOG.md`) get faster to reproduce and
easier to play through end-to-end.

## Context: what's already there

- `tools/game_probe.cpp` is the de facto "play a real game" entry point
  today, even though it lives under `tools/` — window is a fixed 640x480,
  non-resizable (`SDL_WINDOW_SHOWN | SDL_WINDOW_OPENGL`, no
  `SDL_WINDOW_RESIZABLE`).
- `frontends/standalone/sdl2_unified_backend.{h,cpp}` already implements
  real `SDL_GameController` polling (`PollController()`) into `ZPadState`
  (`core/backend.h`) — but `game_probe.cpp`'s actual play loop never calls
  `Backend::PollInput()` at all. All real player input today goes through a
  separate, keyboard-only path (`SdlKeyToHidButton`/`SdlKeyToAvk` in
  `game_probe.cpp`) that injects synthesized real HID button events/AVK key
  events directly into the guest's own registered callbacks.
- `Sdl2UnifiedBackend::PresentFrame`'s GL viewport is set from the
  backend's own stored `width_`/`height_` (fixed at construction), not the
  SDL window's actual live drawable size — the video quad itself is
  already drawn in normalized 0..1 texture/vertex coordinates, so once the
  viewport tracks the real window size, scaling falls out for free.
- No save/restore-state mechanism exists anywhere in the codebase today.
  Emulator state is spread across: `ArmInterpreter` (registers/flags/mode),
  `Memory` (sparse page-based guest address space), and several
  independent host-side C++ classes each owning their own state
  (`MediaHle`, `IShellHle`, `FileHle`, `ModRuntime`, `Mixer`, HID device
  state, ...) — none of them expose a serialize/deserialize hook yet.

## Phase A — Resolution scaling (2x/3x/4x)

Exit criterion: the player can pick 1x/2x/3x/4x window scale and see the
same 640x480 emulated output stretched cleanly, without affecting anything
the guest app itself observes (still reports 640x480 via `AEEDeviceInfo`/
`IDisplayHle`).

- [x] Decouple "logical/emulated resolution" (640x480, unchanged — what
      `IDisplayHle`/`AEEDeviceInfo` report to the guest) from "window/
      presentation size" (what the real SDL window actually is) — done via
      a real offscreen FBO (`Sdl2UnifiedBackend::fbo_`/`fbo_texture_`,
      always rendered at the fixed logical size, with a real depth
      renderbuffer attached too — the real app's own `GL_DEPTH_TEST`
      usage needs one, matching the same `SDL_GL_DEPTH_SIZE` request
      already made for the window's own context) rather than the
      originally-planned "just retarget the viewport" approach, since
      real app-driven GLES draws (not just the 2D quad) also needed to
      scale correctly, not only the `PushVideoFrame` path
- [x] Make the game window resizable (`SDL_WINDOW_RESIZABLE`), plus
      `Sdl2UnifiedBackend::SetWindowScale` doing an explicit
      `SDL_SetWindowSize` to `640*scale x 480*scale` for the hotkey case
- [x] `Sdl2UnifiedBackend::PresentFrame`: blits the offscreen FBO onto
      the real window using its live drawable size
      (`SDL_GL_GetDrawableSize`) every frame, letterboxed (see next item)
      — supersedes the original plan of retargeting `PushVideoFrame`'s
      own viewport directly
- [x] Preserve aspect ratio (4:3) — `frontends/standalone/letterbox.{h,cpp}`
      (`ComputeLetterboxedViewport`), a pure function covering both the
      pillarbox and letterbox cases plus degenerate inputs
- [x] Runtime hook to change scale: F5-F8 hotkeys (see Phase D) via
      `SetWindowScale` — no `--scale=N` CLI flag added (not needed once
      the window is freely resizable + hotkey-drivable; revisit if
      non-interactive/scripted use ever needs it)
- [x] Test: `tests/letterbox_test.cpp`, 6 cases covering exact-aspect-match,
      pillarbox, letterbox, degenerate inputs, and an exhaustive
      never-exceeds-window-bounds sweep
- [x] Bonus, not originally scoped here: F11 fullscreen toggle
      (`SDL_WINDOW_FULLSCREEN_DESKTOP`) — falls out for free once the
      letterboxed blit handles arbitrary drawable sizes anyway

## Phase B — Save states

Exit criterion: the player can save the exact current emulation state to a
file and reload it later, resuming play from that exact point (including
audio/timers, not just CPU+memory) — specifically to let QA sessions like
the sound investigation (`PHASE8_LOG.md`) capture "right here" instead of
re-driving a whole session to reproduce a bug.

Scoped in two stages since host-side HLE state (timers, active sound
voices, media handles) is real, separate work from guest CPU/memory state
— a stage-one snapshot is useful immediately but will NOT correctly resume
mid-sound-effect or mid-timer; that's stage two's job.

- [ ] **Stage 1 — CPU + guest memory only:**
  - [ ] `ArmInterpreter`: serialize all registers, CPSR/flags, mode
  - [ ] `Memory`: serialize the sparse page map (only allocated pages, not
        the full 4GB address space) — page index + 4KB contents each
  - [ ] Simple versioned binary format (a magic/version header first, so
        stage 2's format can extend it without breaking stage-1-only
        saves)
  - [ ] Wire a save/load hook into `game_probe.cpp`'s main loop (hotkey to
        start; see Phase D for the hotkey trigger)
  - [ ] Document the known gap loudly (both in this file and any UI/log
        output): reloading a stage-1-only save resumes guest code/data
        correctly, but host-side transient state (in-flight timers, active
        Mixer voices, MediaHle notify registrations) is NOT restored —
        expect audio/timer glitches immediately after a stage-1 load until
        the guest naturally re-arms them itself
  - [ ] Test: save then immediately reload should leave `ArmInterpreter`
        register state and a probed set of memory addresses byte-identical
- [ ] **Stage 2 — full-fidelity, host-side HLE state included:**
  - [ ] Give each stateful HLE class (`MediaHle`, `IShellHle`, `FileHle`,
        `ModRuntime`, `Mixer`, HID device state, ...) an explicit
        `Serialize`/`Deserialize` pair — likely a small shared interface
        or free-function pattern, not a virtual base (these classes
        aren't currently related by any common base and don't need to
        become so just for this)
  - [ ] `Mixer`: voice list (sample data can potentially be re-derived
        from the still-registered `MediaHle` object it came from rather
        than duplicated into the save file — confirm this is actually
        safe before assuming it, since a since-`Stop()`'d or reused
        object could make that unsafe)
  - [ ] `IShellHle`: pending timers (callback address, user data,
        remaining ms)
  - [ ] `MediaHle`: `media_by_object_` (decoded sample data is real audio
        data and could be large — consider whether to re-decode from the
        source VFS entry on load instead of embedding it verbatim)
  - [ ] `ModRuntime`: heap allocator state
  - [ ] Test: save/reload mid-sound-effect and confirm the sound actually
        keeps playing correctly afterward (this is the concrete case
        Stage 1 is known to get wrong)
- [ ] Multiple save slots (not just one "the" save state) — QA sessions
      often want several different bug-repro checkpoints alive at once
- [ ] Decide where save files live (a `saves/` dir alongside the ROM? a
      dedicated scratch dir?) — keep them out of the git-ignored
      `research/games/` tree's own concerns, this is tooling output, not
      research material

## Phase C — Controller binding (map keyboard-equivalent actions to a real gamepad)

Exit criterion: playing through `game_probe` with a real Xbox-style
(XInput) controller works exactly as well as keyboard does today — same
real HID button/AVK-key injection pipeline, driven by controller input
instead of (or alongside) keyboard input.

- [ ] Wire `Backend::PollInput()` (already implemented for real
      `SDL_GameController` input, see Context above) into
      `game_probe.cpp`'s main loop — currently never called there
- [ ] Diff each tick's `ZPadState` against the previous tick's to get real
      press/release edges per button (`PollController`'s
      `SDL_GameController` state is level-triggered; the existing
      keyboard path is edge-triggered off real `SDL_KEYDOWN`/`SDL_KEYUP`
      events, so this needs to produce the same shape of edge events for
      the shared injection code below to reuse)
  - [ ] Include the two analog sticks — decide a real deadzone and a
        direction threshold that maps stick tilt to the same real D-pad
        HID UIDs `SdlKeyToHidButton` already uses for keyboard arrows,
        since Double Dragon's own real input handling (confirmed earlier
        in this investigation) only recognizes the D-pad UIDs, not analog
        axes
- [ ] A real `ZPadState` button -> (real HID button UID / real AVK code)
      mapping table, mirroring `SdlKeyToHidButton`/`SdlKeyToAvk`'s
      existing real-UID mappings — both keyboard and controller should
      feed the exact same downstream injection call, not diverge into two
      codepaths
- [ ] Sensible default bindings (face buttons -> attack buttons, D-pad ->
      D-pad, Start -> the confirmed real `kBack`/title-progression
      button, shoulders -> the two real shoulder UIDs already mapped) —
      reuses the real UID meanings `SdlKeyToHidButton`'s own doc comment
      already established, not new guesses
- [ ] Support keyboard and controller simultaneously (don't require
      picking one) — both should just work, matching how most emulators
      behave
- [ ] Test: a fake/injected `ZPadState` sequence through the
      edge-detection logic produces the expected press/release event
      sequence (this part needs no real SDL controller hardware to test)

## Phase D — Hotkeys + on-screen overlay (decided: no ImGui toolbar)

Exit criterion: the three features above are reachable live, without
editing code or restarting with different CLI flags, via hotkeys plus a
transient on-screen text overlay for feedback/status.

Decision: hotkeys + the existing text-overlay precedent, not a real
immediate-mode GUI library — cheapest, fastest to ship, no new
dependency, and it reuses `Sdl2UnifiedBackend::DrawFpsOverlay`'s already-
working transient-text-over-GL approach directly rather than building a
second, separate rendering path alongside it.

- [x] Hotkey table (F5-F8 scale, F9 overlay toggle, F11 fullscreen —
      documented in README.md's own Controls section; none collide with
      any real key `SdlKeyToAvk`/`SdlKeyToHidButton` forwards to the guest,
      since none of those use function keys):
  - [x] Cycle/select window scale (1x/2x/3x/4x, Phase A) — F5/F6/F7/F8
  - [ ] Save state / load state, with some way to pick a slot once Phase
        B has more than one (even if just "hold + number key" at first)
  - [ ] Enter/exit a controller-rebinding mode (Phase C)
  - [x] **Toggle the overlay's visibility itself** — F9
        (`SetOverlayVisible`), independent of the other hotkeys, so it
        can be turned off entirely once the player's just playing
- [x] Extended the old `DrawFpsOverlay` into `DrawOverlay` (still the
      same save/restore-GL-state pattern), with a `DrawText` helper and
      a second, transient status-message line (`ShowStatusMessage`,
      auto-clears after ~2.5s) rather than a second one-off text-drawing
      path — also extended the tiny 3x5 bitmap font with the letters
      status messages actually need (A/C/D/E/H/I/L/N/O/R/S/T/U/V/W/X)
- [x] Overlay-visible state persists across the hide toggle correctly —
      `DrawOverlay` returns immediately (draws nothing, including the FPS
      line) while `overlay_visible_` is false
- [ ] Persist settings (scale, bindings, and whether the overlay starts
      shown or hidden) across runs — a small config file (JSON/INI) next
      to wherever save states end up living (Phase B)

---

Not a replacement for `TASKS.md`'s own phases — this file is purely the
player-facing QA/dev-experience tooling the user asked for after Sound,
round fifteen (`PHASE8_LOG.md`) wrapped up, to make the next round of
manual bug-hunting faster.
