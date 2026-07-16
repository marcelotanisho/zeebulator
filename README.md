# Zeebulator

A free, open-source, high-level-emulation (HLE) emulator for the
[Zeebo](https://en.wikipedia.org/wiki/Zeebo) game console — a Qualcomm
MSM7201A / BREW-based console sold in Brazil, Mexico, and Indonesia
(2009–2011). Targets native builds on Windows, macOS, and Ubuntu/Linux,
plus a [libretro](https://www.libretro.com/) core for RetroArch.

**Status: early development.** There is no working emulation yet — this
repo currently has a buildable project skeleton and a CPU/memory core in
progress. See [TASKS.md](TASKS.md) for where things stand.

## Why HLE?

Zeebo isn't fixed-function console hardware — it's a Qualcomm MSM7201A
(ARM1136/ARM11 @ 528 MHz) running **Qualcomm BREW 4.0.2** as its OS layer,
the same mobile runtime used on feature phones of that era. Games are
native ARM code written against BREW's C/C++ APIs plus OpenGL ES 1.0/1.1.
Zeebulator reimplements the BREW API surface in high-level emulation
rather than emulating the SoC at the hardware level — which also means
**no copyrighted firmware/BIOS is required** to run it. You just need your
own legally-obtained game dumps.

## Building

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

Build options (all default `ON`):
- `ZEEBULATOR_BUILD_STANDALONE` — SDL2 dev/debug frontend
- `ZEEBULATOR_BUILD_LIBRETRO` — libretro core
- `ZEEBULATOR_BUILD_TESTS` — test suite (GoogleTest)

## Project docs

- [PRD.md](PRD.md) — goals, scope, milestones
- [ARCHITECTURE.md](ARCHITECTURE.md) — component design, directory layout, technology choices
- [TASKS.md](TASKS.md) — phased task breakdown and current progress
- [CONTRIBUTING.md](CONTRIBUTING.md) — clean-room policy (read before contributing) and dev setup

## Legal

This project distributes no Qualcomm, BREW, or Zeebo/Tectoy copyrighted
material — no firmware, no SDK files, no game data. You are responsible
for supplying your own legally-obtained game dumps. Zeebulator is an
independent, fan-made project with no affiliation to Zeebo Inc., Tectoy,
or Qualcomm.

## License

[GPLv3](LICENSE).
