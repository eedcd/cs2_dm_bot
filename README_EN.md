# CS2 DM Bot

[![Build](https://github.com/eedcd/cs2_dm_bot/actions/workflows/build.yaml/badge.svg?branch=main)](https://github.com/eedcd/cs2_dm_bot/actions/workflows/build.yaml)
[![Offsets](https://github.com/eedcd/cs2_dm_bot/actions/workflows/update-files.yml/badge.svg?branch=main)](https://github.com/eedcd/cs2_dm_bot/actions/workflows/update-files.yml)
[![License](https://img.shields.io/github/license/eedcd/cs2_dm_bot)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%2F11-0078D4)
![C++](https://img.shields.io/badge/C%2B%2B-20-00599C)

An external CS2 deathmatch bot built with SDL2, Dear ImGui, and C++20. There is no interactive menu: only a top-right status badge. When enabled, it runs ESP, aimbot, triggerbot, and map-aware movement automatically.

[中文](README.md) | **English**

> [!WARNING]
> This project is intended only for learning reverse engineering and graphics rendering. Run it only in an offline environment launched with `-insecure`. Do not use it on VAC servers or in any online game. You are responsible for the consequences of misuse.

## Learning Resources

| Topic | Video | Documentation |
| --- | --- | --- |
| Memory structures | [BV14szCBYErE](https://www.bilibili.com/video/BV14szCBYErE) | [CS2 memory structures](docs/CS2_Memory_Structure_Basics.md) |
| Source and CE walkthrough | [BV1Jm6gBaEEd](https://www.bilibili.com/video/BV1Jm6gBaEEd) | [Code walkthrough](docs/cheat_code.md) |
| Finding offsets | [BV1Lr6wBeEEF](https://www.bilibili.com/video/BV1Lr6wBeEEF) | [IDA reverse engineering](docs/IDA_Reverse_Engineering_Client_DLL.md) |
| Projection matrices | [BV1goFNzSEP3](https://www.bilibili.com/video/BV1goFNzSEP3) | [3D projection](docs/3d_projection_explained.md) |
| View matrices | [BV1AtF5zhE5J](https://www.bilibili.com/video/BV1AtF5zhE5J) | [View-matrix math](docs/cs2_view_matrix_math.md) |
| Signatures and IDA | [BV1yhcszwEJQ](https://www.bilibili.com/video/BV1yhcszwEJQ) | - |

## Features

| Component | Capabilities |
| --- | --- |
| Status badge | Always-on-top in the top-right corner; yellow `DM Bot waiting for CS2 process` without CS2, blue `DM Bot disable` / `DM Bot enable` once attached |
| ESP | Boxes, skeletons, health, distance, weapon, view angle, visibility; **all teams**, every CT / T except yourself |
| Aimbot | Active while enabled; 360-degree search, nearest angle first, only real visible targets (layered map line-of-sight, no wall locking) |
| Triggerbot | Active while enabled; fires 100–300 ms after locking a target |
| Movement | A\* pathfinding over radar walkable masks, prefers the nearest living player, patrols otherwise, detours around walls, counter-strafes before firing |
| Map data | 16 common maps plus 4 layered lower floors (Nuke / Train / Vertigo / Baggage) |

## Requirements

- Windows 10 or Windows 11 (x64)
- Visual Studio 2022 (or newer) with the Desktop development with C++ workload
- CS2 launched with `-insecure` in Windowed Fullscreen mode

The overlay supports a game window located entirely on one monitor. Rendering pauses while the window spans monitors and resumes when it moves back. Exclusive fullscreen is unsupported. The application requests administrator privileges at startup and reconnects automatically after CS2 restarts.

## Quick Start

1. Download and extract the latest package from [Releases](https://github.com/eedcd/cs2_dm_bot/releases/latest).
2. Make sure the `maps\` folder sits next to `external-cheat-base.exe` (included in the archive).
3. Launch CS2 with `-insecure` and enter an offline match.
4. Run `external-cheat-base.exe` and accept the Windows administrator prompt.

| Key | Action |
| --- | --- |
| `F4` | Toggle between `DM Bot disable` and `DM Bot enable` |
| `F9` | Exit the application |

While enabled (`DM Bot enable`), ESP, aimbot, triggerbot, and automatic movement run immediately. While disabled, every feature stops and movement keys are released.

The memory-writing Anti-Flash option is disabled by default. Enable memory writes only for explicit offline testing:

```powershell
.\external-cheat-base.exe --allow-memory-writes
```

Runtime logs are written to `%TEMP%\cs2-esp.log`.

## Navigation and Line of Sight

- Radar images (`assets/maps/*.png`) only paint walkable floor, so they are used directly as a navigation grid:
  - 512×512 grid, one-cell erosion for wall clearance, 8-direction A\*, line-of-sight path smoothing.
  - The current map name is read from `dwGlobalVars`; its `originX / originY / scale` converts world coordinates to pixels and back.
  - Paths are recomputed every 600 ms or when the goal moves more than 250 units; movement stops within 150 units of the target.
- Layered maps select the upper or lower mask by Z threshold; when the endpoints are on different floors, both masks are checked so stairs and ramps still count as visible.
- Wall detection casts one ray each to the head, chest, and feet, and treats the target as visible if any point is clear.
- An external process cannot call the game's `TraceShape`, so visibility is approximated from radar walkable masks; VPK 3D collision geometry is not parsed.

Maps outside the bundled list fall back to "chase the nearest player + detour around walls".

## Building

### Visual Studio

```powershell
git clone https://github.com/eedcd/cs2_dm_bot.git
```

Open `external-cheat-base.sln` in Visual Studio, select `Release | x64`, and build. The post-build step copies `SDL2.dll` and `assets/maps` into the output directory.

Command line:

```powershell
msbuild external-cheat-base.sln /p:Configuration=Release /p:Platform=x64
```

The result is in `x64\Release\`; the `maps\` folder must stay next to the executable.

## Project Structure

| Path | Purpose |
| --- | --- |
| `external-cheat-base/` | Windows C++ application, overlay, ESP, aimbot, triggerbot, and movement |
| `assets/maps/` | Radar masks (16 maps + 4 lower floors), copied to the output at build time |
| `tests/` | C++ regression tests (viewport math, sampling timing) |
| `docs/` | Reverse-engineering and rendering documentation |
| `.github/workflows/` | Build, release, and automatic offset updates |

Key modules:

- `src/features/esp.cpp`: entity reading and ESP rendering
- `src/features/aimbot.cpp`: aimbot, triggerbot, and visibility checks
- `src/features/movement.cpp`: automatic movement, quick stop, and wall detours
- `src/features/navigation.cpp`: radar mask decoding, A\* pathfinding, layered line-of-sight
- `src/main.cpp`: status badge, F4/F9, and the data/render loops

## CI and Offset Updates

- The **Build** badge is generated by GitHub Actions: green means the latest `main` build passed.
- CI runs C++ regression tests on Ubuntu, then compiles `Release | x64` on Windows, verifies the release layout, and publishes a release.
- The **Offsets** workflow checks [cs2-dumper](https://github.com/a2x/cs2-dumper) hourly. It fetches and validates headers at an exact commit SHA, commits them as `build: update CS2 offsets to <sha>`, and then triggers the full build.

## License and Third-Party Assets

Project code is released under the [MIT License](LICENSE).

Radar images and overview coordinates are Valve property and remain subject to Valve's copyright and terms. Primary dependencies and data sources: [cs2-dumper](https://github.com/a2x/cs2-dumper), [SDL](https://github.com/libsdl-org/SDL), [Dear ImGui](https://github.com/ocornut/imgui), and [awpy-data](https://github.com/pnxenopoulos/awpy-data).
