AmnesiaAMFP.FPSFix 1.0.0
========================

AmnesiaAMFP.FPSFix lets Amnesia: A Machine for Pigs render above 60 FPS while
the game itself keeps running on its original fixed 60 Hz update. It provides
presentation-rate camera movement and mouse look, menu and journal cursors,
first-person hands and lantern, physics props, doors, drawers, levers,
machinery, Pig Men and ghost children, ragdolls, particles, and
frame-rate-correct flashback afterimages.

COMPATIBILITY
-------------

The game executable must be a 32-bit Windows / PE32 executable that uses SDL2,
OpenGL, and Newton and whose required engine functions uniquely match every
validated code signature. The plugin does not require a particular file name,
file hash, storefront, installation path, file size, or ASI loader. If a
required mapping is absent or ambiguous, startup fails closed before any
engine hook is installed and the reason is written to the log. This scan runs
once at startup.

Validated release build:

GOG Windows build ID: 51194033869387641
aamfp.exe SHA-256:
9FAE927B3B37D37C7E087548D895581A3AF2EC541019F4C2A81D8392A5C8BE8B

Other storefronts and executable revisions are allowed by the compatibility
scanner but are untested.

Tested on the main game from the mansion through the first Pig Man encounter
at roughly 230 to 350 FPS. Later levels, the Water Lurker and other enemy
types, custom stories, and commentary mode are untested; anything the plugin
does not recognise keeps its original 60 Hz motion.

INSTALLATION
------------

All files go in the "redist" folder beside aamfp.exe, not in the parent game
folder.

Plugin-only package:

1. Install a compatible x86 ASI loader for the game.
2. Copy AmnesiaAMFP.FPSFix.asi and AmnesiaAMFP.FPSFix.ini together into
   "redist", or into another directory scanned by that loader.

Package bundled with Ultimate ASI Loader:

1. Copy AmnesiaAMFP.FPSFix.asi, AmnesiaAMFP.FPSFix.ini, and wininet.dll into
   "redist".
2. Do not overwrite an existing wininet.dll. If one is already present, find
   out which mod owns it and use the plugin-only package with that loader
   instead.

The bundled wininet.dll is the verified x86 Ultimate ASI Loader v9.7.4 binary.
The game is 32-bit, so a 64-bit loader will not work.

Start the game as usual. Launcher.exe also loads the plugin, which logs that
the launcher does not look like the game executable and does nothing there;
that message is expected.

USAGE
-----

The plugin does not limit the frame rate. Enable the game's VSync or use an
external frame-rate cap, otherwise the game renders as fast as the machine
allows. The simulation stays at 60 Hz by design, so higher frame rates do not
speed the game up.

The supplied INI enables every validated fix, and each one has its own switch.
Most users should leave it unchanged.

The log is written beside the plugin as AmnesiaAMFP.FPSFix.log. If that
location is not writable, the fallback is:

%LOCALAPPDATA%\AmnesiaAMFP.FPSFix\AmnesiaAMFP.FPSFix.log

UNINSTALLATION
--------------

Remove AmnesiaAMFP.FPSFix.asi and AmnesiaAMFP.FPSFix.ini. If this package
added wininet.dll, remove it only when no other installed ASI plugin depends
on that loader. Log files can be removed separately.

FILES AND THIRD-PARTY SOFTWARE
------------------------------

SHA256SUMS.txt lists hashes for every payload file. MINHOOK-LICENSE.txt covers
the statically linked MinHook dependency. The bundled-loader package also
includes THIRD-PARTY-NOTICES.txt for Ultimate ASI Loader.

This software is provided without warranty. Back up modified game files and
save data before installing game modifications.


Misc
-------
For more game fixes, visit https://slop-blog.enkhayzomachines.net/fixes :)
