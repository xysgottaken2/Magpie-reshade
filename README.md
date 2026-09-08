# Magpie ReShade Input Bridge

A small ReShade add-on for **SAOG0721/Magpie Experimental** that temporarily gives the ReShade overlay access to mouse and keyboard input while Magpie is running.

## Why this exists

Magpie's renderer normally uses input-related window styles that make interacting with a ReShade overlay difficult. This add-on provides a separate input mode without modifying Magpie's binaries.

## Controls

**F10** toggles ReShade interaction mode.

When enabled, the add-on:

- locates Magpie's renderer window;
- temporarily changes the renderer's input-related extended styles;
- releases mouse clipping and gives the renderer focus;
- opens the ReShade overlay through ReShade's add-on API;
- keeps the original window state so it can be restored afterward.

Press **F10** again to leave interaction mode. The overlay is closed through the ReShade API and the renderer's original styles are restored.

## How the overlay is opened

The add-on does **not** simulate ReShade's configured overlay key. Instead, it obtains ReShade's active `effect_runtime` and calls `effect_runtime::open_overlay()` from ReShade's `reshade_present` event.

This means the add-on does not depend on `KeyOverlay` in `ReShade.ini`.

## Requirements

- Windows x64
- ReShade 6.8.x 64-bit with add-on support
- SAOG0721/Magpie Experimental x64
- An MSVC-compatible C++17 toolchain
- CMake

## Build

Run the included build script:

```powershell
.\build.ps1
```

The script prepares the required ReShade headers and builds the add-on.

## Installation

Build the project and rename the resulting DLL to:

```text
MagpieReShadeInput.addon64
```

Place it in Magpie's add-on directory, for example:

```text
Magpie-Experimental-x64\addons\
```

Your `reshade.ini` should contain:

```ini
[ADDON]
AddonPath=.\addons
```
⚠️ You will have to drag others addons(.addon64) to that same folder

Restart Magpie after installing the add-on.

## Using it

1. Start Magpie with ReShade loaded.
2. Press **F10**.
3. Interact with the ReShade overlay normally using the mouse and keyboard.
4. Press **F10** again to return to normal Magpie/game input.

ReShade settings and presets can be edited and saved normally while interaction mode is active.

## Notes

This is an experimental add-on built specifically around Magpie's current renderer/window behavior. Changes to Magpie's window architecture may require updates to the detection or input handling code.

The default toggle key is defined as `VK_F10` in `src/MagpieReShadeInput.cpp`. If F10 conflicts with the application being scaled, the source can be changed and rebuilt.

The add-on only changes the identified renderer window and restores its previous styles when interaction mode is disabled or the add-on is unloaded.

## Credits / Inspiration

The input-passthrough approach was inspired by the general idea behind **LSP-ReShade**. This project is an independent implementation for Magpie Experimental and does not include Lossless Scaling or LSP-ReShade source code.

## Disclaimer

This is unofficial software and is not affiliated with or endorsed by Magpie, ReShade, or their respective developers.

Use it at your own risk, especially with software that uses anti-cheat or other mechanisms that react to injected add-ons or window manipulation.
