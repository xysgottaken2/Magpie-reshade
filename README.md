# Magpie ReShade Input Passthrough

Experimental ReShade add-on for SAOG0721/Magpie Experimental.

## What it does

Magpie's renderer window is deliberately created with `WS_EX_NOACTIVATE`, which makes it awkward for ReShade's overlay to receive keyboard/mouse input.

This add-on provides a separate toggle:

**F10** = enable/disable ReShade input mode.

When enabled it:

1. Finds `Magpie_Renderer`.
2. Removes `WS_EX_NOACTIVATE` from that window.
3. Brings the Magpie renderer to the foreground.
4. Sends ReShade's `Home` key to open the ReShade overlay.
5. Keeps the renderer focusable so the overlay can receive input.
6. Press F10 again to close the overlay and restore the original styles/focus.

The add-on does not modify Magpie binaries.

## Requirements

- Windows x64
- ReShade 6.8.x 64-bit with add-on support
- SAOG0721/Magpie Experimental x64
- Visual Studio 2022 or another MSVC-compatible C++17 toolchain
- CMake
- Git

## Build

Run:

```powershell
.\build.ps1
```

The script downloads the ReShade 6.8.0 headers and builds the add-on.

## Install

Rename:

`MagpieReShadeInput.dll` to `MagpieReShadeInput.addon64`

and put it at the directory ./addons

Then restart Magpie.
(Make sure you have
[ADDON]
AddonPath=.\addons
In reshade.ini)

## Usage

Start Magpie scaling with ReShade loaded.

Press:

**F10**

If everything works, the ReShade overlay should open.

Press F10 again to close it and return focus to the source/game window.

## Important

This is an experimental first version. It is specifically designed around the `Magpie_Renderer` window and may need adjustment if SAOG0721 changes its window/input architecture.

If F10 is already used by the game, change `TOGGLE_KEY` in `src/MagpieReShadeInput.cpp`.

Do not test this first on an important game with anti-cheat. It changes window focus/styles and simulates a keypress inside the local process.
