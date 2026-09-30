# CameraPlus - A Closer, Tunable Camera

A mod for CONTROL Resonant: a closer, over-the-shoulder third-person camera, with its own cameras for exploration, indoors, combat, zoom and idle, all tuned live in a panel in the game.

Download and install instructions are on Nexus Mods (search for CameraPlus in the CONTROL Resonant section). This repository is the full source.

## Features

- A panel on the left of the screen (F1, or D-pad Left on a controller) to tune the camera live: distance, look-at height, side offset, camera position and field of view.
- Styles to start from: Game default, Over the shoulder, Close shoulder, Cinematic and Zoom.
- Separate cameras for exploration, indoors (where the game switches to its tighter indoor camera; the same as exploration until you give it its own), combat (the game's own by default, or the same as exploration), zoom (a key or controller button you pick, hold or toggle) and idle (a slow zoom in when you stand still).
- Jump camera: in the air the game switches to a camera that drops lower and aims below the player, which puts a close camera's player near the top of the screen. Each camera keeps as much of it as you set, from 0% (the camera stays as it was on the ground) to 100% (the game's).
- The game's own camera does the rest: collision, the pivot, jumps, dashes, gravity and lock-on all work on the new values, and every blend between the game's camera sets stays smooth.
- Keyboard and controller support; while the panel is open the game doesn't see those presses, but the mouse and sticks still move the camera so you can look at a change as you make it.
- The panel key and button can be changed on the MODS page (Mod Settings Menu).

## Requirements (to play)

- [f2g DLL Mod Loader (crloader)](https://www.nexusmods.com/controlresonant/mods/9)
- [Mod Settings Menu](https://www.nexusmods.com/controlresonant/mods/35) (for the MODS page options)

## Building

Requires Windows and the Visual Studio 2022 Build Tools (C++ workload). Run `build.bat`; it builds `build\cameraplus.dll`. To install your build, put it in `crmods\CameraPlus` in the game folder together with the files in `mod\`.

## How it works

`cameraplus.dll` is loaded by [f2g DLL Mod Loader (crloader)](https://www.nexusmods.com/controlresonant/mods/9) from `crmods\CameraPlus`. At start-up it reads the game executable from disk, finds the game functions it needs by byte signatures, and installs a few inline hooks inside the game process only.

- **The camera:** the game blends its camera sets (distances, positions, field of view) in `heron::cameraset::update_blending`. CameraPlus writes its changes into the set the blend is heading to, so every copy the game makes from there (the blend itself, where the next blend starts, and the tail camera's history of recent states) carries them, and the game's own tail camera, collision and blending do the rest.
- **The jump camera:** `select_set` records the movement a set is for (jump, sprint, dash...). While it's the jump, the jump set's field of view, distance and positions are taken only part of the way from the set used on the ground before it.
- **The indoor camera:** camera state zones in the levels set the game's camera state as the player walks into them; while it's the indoor state, CameraPlus uses its Indoor camera.
- **The panel:** `CameraPlus.js` is appended to the game's UI bundle when the game loads it and draws the panel from the DLL's status through a `coui://` endpoint. The keyboard is read from the game window's raw input; controllers through XInput and, for DualSense and DualShock 4, their HID reports. While the panel is open, the game's own controller button checks answer "not pressed".

There is no network code; the mod writes only its own log and settings files in its own folder.

## Antivirus false positives

Some antivirus tools, including Windows Defender (`Trojan:Win32/Wacatac`), may flag the DLL because it is unsigned and hooks into the game. This is a false positive and has been reported to Microsoft. The full source is here, so you can check it or build the DLL yourself with `build.bat`.

## Privacy policy

This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it.

## Credits

- **fame2gin** for f2g DLL Mod Loader.
- **kkyleeb21** for Mod Settings Menu, and MapFusion, which showed how to add scripts to the game's UI.
