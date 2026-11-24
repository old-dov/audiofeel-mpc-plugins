# Windows editor for the VST2 builds

The SDK's VST2 for a computer is generic: the host shows a plain list of parameters. This folder adds a
window to it, drawn from each plugin's `ui.json` (the same pages, knobs, faders, switches and menus as on
the MPC screen), without changing the SDK or the plugin's source.

- Windows only, 64-bit VST2. The MPC builds are not affected.
- One accent colour per plugin (`ACCENTS` in `gen_layout.py`).
- Plugins whose pages fit in four rows show them all at once; the others get a tab per page.
- Knob and fader: vertical drag (Shift = fine), mouse wheel, double-click = default value.
  Switch: click. Menu: click, then pick.

## Build

Needs the [MPC plugin builder SDK](https://github.com/inmusic-software/mpc-plugin-builder-sdk) with these
plugins copied into its `plugins/` folder (see the main README) and its bundled Zig toolchain.

```
set MPC_SDK=C:\path\to\mpc-plugin-builder-sdk
python build_gui.py Arpeggio          (or: all)
```

The result is `out/<Name> GUI/<slug>-gui.dll` with `plugin.json`. Rename the DLL to the plain plugin's
name to replace it, or keep both folders apart (the two share one plugin ID: load only one per project).

## How it works

`shim/mpc_plugin.h` is found before the SDK's header, includes it, then redefines `MPC_PLUGIN_ENTRY` so that
the effect gets an editor (`af_editor_win.h`): it answers `effEditGetRect`, `effEditOpen`, `effEditClose` and
raises `effFlagsHasEditor`. The window is a Win32 child window drawn with GDI+. `gen_layout.py` turns
`ui.json` into the layout the editor uses.

`test_host.cpp` is a tiny VST2 host for trying an editor without a DAW (opens it, drags a knob, saves a
screenshot).

## Status

Tried in Ableton Live 12 on Arpeggio; the fourteen plugins were opened and driven in the test host.
