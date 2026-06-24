**C4dll-R __VER__** - standalone DisciplesGL-style renderer for Disciples II (Russobit), built from cnc-ddraw, with the reconstructed native turn timer. Independent of the mss32 mod.

**Install:** download `__ZIP__`, read `INSTALL.txt`, drop the files next to `Discipl2.exe`.

Contents:
- `C4dll-R.dll` - the renderer monolith (embedded cnc-ddraw + CodeBase forwarder + in-game menu)
- `Mods/timer.c4p` - native turn timer; the host resets it on every turn change, including a skipped turn
- `C4plugins.ini` - timer settings (countdown on by default)
