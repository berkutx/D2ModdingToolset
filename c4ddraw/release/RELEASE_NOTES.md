**C4dll-R __VER__** - standalone DisciplesGL-style renderer for Disciples II (Russobit), built from cnc-ddraw, with the reconstructed native turn timer. Independent of the mss32 mod.

**Install:** download `__ZIP__`, read `INSTALL.txt`, drop the files next to `Discipl2.exe`.

Contents:
- `C4dll-R.dll` - the renderer monolith (embedded cnc-ddraw + CodeBase forwarder + in-game menu)
- `Mods/timer.c4p` - native turn timer; the host resets it on every turn change, including a skipped turn
- `C4plugins.ini` - timer settings (countdown on by default)

Highlights in this release:
- simple `Ctrl+Mouse Wheel` zoom, anchored at the cursor, for OpenGL, D3D9/Auto and GDI
- Scenario Editor menu with Scenarios/Campaigns database switching and explicit EN/RU/automatic menu language
- selectable game/editor text locale compatible with the legacy wrapper's `[Wrapper] Locale`
- version-independent quick-save/archive handling and safer window-edge scrolling
- optional unit-hire auto-confirmation (off by default) and more reliable click-vs-drag selection
- quieter release defaults: diagnostic logs are written only when explicitly enabled
