# AmnesiaAMFP.FPSFix

> [!WARNING]
> This patch has been pretty much entirely been generated using AI; 
> I won't and will never claim to have enough knowledge or expertise to do this 
> kind of reverse-engineering by myself; I've tested about 5 levels (reached the church) at 360hz + 
> briefly tested at 120, 144 and 240 and so far spotted 
> no major issues (on my machine ofc, if you have issues feel free to open an Issue).
> If you're interested and want more (human generated) info, I have a blog post [here](https://slop-blog.enkhayzomachines.net/posts/amnesia-a-machine-for-pigs-high-fps-ril) :)

A high-frame-rate fix for the Windows version of *Amnesia: A Machine for Pigs*.
It lets the game render above 60 FPS while physics, scripts, AI, saves, and
other gameplay systems stay on the engine's original 60 Hz update loop.

Unlike *The Dark Descent*, this game only draws a frame after a game update,
so turning off its frame limiter alone changes nothing. The plugin removes that
restriction and then smooths what would otherwise still move in 60 Hz steps.

## Features

- Smooth camera movement, raw mouse look, and menu/journal cursors
- Interpolated first-person hands and lantern, including its light
- Smooth physics props, doors, drawers, levers, wheels, and animated machinery
- Smooth Pig Men and ghost children (animation and movement) and ragdolls
- Interpolated particles and frame-rate-correct flashback afterimages
- Automatic compatibility checks that fail safely before modifying the game
- A separate switch for every feature and a lightweight log

The simulation rate is never increased, so high frame rates do not speed up
the game.

## Tested

The fix was tested on the GOG build from the mansion through the first Pig Man
encounter at roughly 230 to 350 FPS. Later levels, the Water Lurker and other
enemy types, custom stories, and commentary mode are untested. Anything the
plugin does not recognise keeps its original 60 Hz motion.

## Requirements

- A 32-bit Windows copy of *Amnesia: A Machine for Pigs*
- A compatible x86 ASI loader (one of the release archives includes it)

The validated release target is the GOG Windows build:

| | Value |
| --- | --- |
| GOG build ID | `51194033869387641` |
| `aamfp.exe` SHA-256 | `9FAE927B3B37D37C7E087548D895581A3AF2EC541019F4C2A81D8392A5C8BE8B` |

Other storefronts and revisions may work. On startup the plugin verifies every
required engine signature and import instead of relying on the executable's
name, hash, or install path. If the mapping is incompatible or ambiguous, it installs
no hooks and records the reason in the log.

## Installation

Everything goes in the game's `redist` folder, beside `aamfp.exe`, not in the
parent game folder.

### Bundle with Ultimate ASI Loader

1. Open the archive ending in `with-Ultimate-ASI-Loader.zip`.
2. Copy its contents into `redist`.
3. Start the game normally.

Do not overwrite an existing `wininet.dll`. If one is already present, another
mod may own it; use the plugin-only package with that mod's x86 ASI loader
instead.

### Plugin-only package

1. Install a compatible x86 ASI loader for the game.
2. Copy `AmnesiaAMFP.FPSFix.asi` and `AmnesiaAMFP.FPSFix.ini` together into
   `redist`, or into another directory scanned by that loader.
3. Start the game normally.

The plugin always looks for its INI beside the ASI.

## Usage

The plugin does not limit the frame rate. Enable the game's VSync or use an
external frame-rate cap; otherwise the game renders as fast as the machine
allows.

The supplied configuration enables every validated fix. Settings are
documented in [`config/AmnesiaAMFP.FPSFix.ini`](config/AmnesiaAMFP.FPSFix.ini);
most users should leave them unchanged. If something looks wrong, turning off
the matching feature there returns that part of the game to stock behaviour.

The log is written to `AmnesiaAMFP.FPSFix.log` beside the plugin. If that
directory is not writable, it is placed in:

```text
%LOCALAPPDATA%\AmnesiaAMFP.FPSFix\AmnesiaAMFP.FPSFix.log
```

`Launcher.exe` also loads the plugin, which logs that the launcher does not
look like the game executable and does nothing there. That message is
expected.

When reporting a problem, include the log and note the storefront, ASI loader,
display refresh rate, and frame cap or VSync setting.

## Uninstallation

Remove `AmnesiaAMFP.FPSFix.asi` and `AmnesiaAMFP.FPSFix.ini`.

If you installed the bundled `wininet.dll`, remove it only if no other ASI mod
uses that loader. Logs can be removed separately.

## Building from source

Requirements:

- Visual Studio with the MSVC x86 C++ toolchain
- CMake 3.24 or newer
- Ninja (for the commands below)

From an x86 Visual Studio developer environment:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
ctest --test-dir build --output-on-failure
```

The plugin and default INI are written to `build/bin`. To create the
plugin-only release archive:

```powershell
cmake --build build --target package_plugin_only
```

The `package_ultimate_asi_loader` target builds the bundle when the pinned
loader binary is present locally; it is not part of this repository. Packages
are written under `dist`.

## How it works

The engine runs gameplay on a fixed 60 Hz clock. The plugin lets the renderer
draw between those updates, records the state the game produced at each one,
and temporarily shows supported objects part-way between the last two states
while a frame is drawn. The game's own values are put back as soon as the frame
is done, so nothing the simulation reads is ever changed.

Mouse look is the one exception to "between the last two states": new mouse
movement is shown on the camera immediately and handed to the next game update
exactly once, so looking around does not lag behind by an update.

## License

AmnesiaAMFP.FPSFix is released under the [MIT License](LICENSE). It statically
links [MinHook](third_party/minhook/LICENSE.txt), which is distributed under
the 2-clause BSD license.
