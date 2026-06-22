/*
 * C4dll-R plugin API v2 - the "new" 32bpp-native plugin format.
 *
 * A plugin is a DLL placed in <game>\mods\ with extension ".c4p". It draws a transparent BGRA32
 * overlay that C4dll-R composites over the game frame (the game itself renders at 8bpp palettized,
 * so plugins never touch the game surface - they own a separate overlay layer).
 *
 * Efficiency (vs the legacy .mod format): the host calls c4p_draw ONLY when the plugin marks itself
 * dirty via host->invalidate(), and composites the cached overlay texture every frame. So an
 * expensive redraw (GDI+, text shaping) happens only when content actually changes (e.g. a turn
 * timer ticks once per second), not every frame. Per-frame logic goes in the cheap c4p_tick.
 *
 * Turn detection is HOST-driven (the host owns all game coupling, the plugin stays portable): the
 * host watches the game's current-turn player and exposes it via C4P_Host (get_turn_serial bumps on
 * every turn change, including a skipped turn). A reconstructed turn timer just resets its countdown
 * when the serial changes - it never touches the game itself.
 *
 * We build our own plugins (e.g. the reconstructed turn timer) to this format. The legacy .mod
 * format is still loaded for backward compatibility, but redraws every frame and is meant only for
 * users who do not update their plugins. A new plugin may declare supersedes_legacy_id to replace a
 * specific legacy .mod when both are present (the host then drops the legacy one - no double draw).
 */
#ifndef C4PLUGIN_H
#define C4PLUGIN_H

#include <stdint.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define C4P_ABI_VERSION 2u

#ifdef __cplusplus
extern "C" {
#endif

/* The overlay the plugin draws onto: BGRA8888 (GDI+ PixelFormat32bppARGB), top-down, sized to the
 * game client area. STRAIGHT (non-premultiplied) alpha - the host premultiplies for compositing.
 * Already cleared to fully transparent before each c4p_draw. */
typedef struct C4P_Canvas
{
    uint32_t struct_size; /* sizeof(C4P_Canvas) */
    void* pixels;         /* BGRA32 top-down */
    int32_t width;
    int32_t height;
    int32_t stride; /* bytes per row */
} C4P_Canvas;

/* Services the host exposes to the plugin (lifetime = the plugin's lifetime). */
typedef struct C4P_Host
{
    uint32_t struct_size; /* sizeof(C4P_Host) */
    HWND(__cdecl* get_hwnd)(void);
    void(__cdecl* invalidate)(void); /* ask the host to call c4p_draw on the next frame */
    int(__cdecl* get_config_int)(const char* section, const char* key, int def);
    void(__cdecl* set_config_int)(const char* section, const char* key, int value);
    const char*(__cdecl* config_path)(void); /* full path of the shared C4plugins.ini next to the exe */

    /* ---- turn state (host-driven turn detection), added in ABI v2 ----
     * get_turn_serial: a counter the host bumps on every detected turn change (player change,
     *   including a skipped turn). A timer resets its countdown whenever this value changes.
     * get_turn_player: the current turn's player index (>=0), or -1 when unknown / not in a game.
     * is_in_game: 1 while a scenario turn is in progress (so a timer can hide outside a game). */
    uint32_t(__cdecl* get_turn_serial)(void);
    int(__cdecl* get_turn_player)(void);
    int(__cdecl* is_in_game)(void);

    /* ---- battle state, appended after ABI v2 (guarded by struct_size, no version bump) ----
     * is_in_battle: 1 while a battle viewer is on screen. Backs the timer's Combat Pause. A plugin
     *   must check `struct_size >= sizeof(C4P_Host)` before calling, so an older host (smaller
     *   struct) is handled gracefully. */
    int(__cdecl* is_in_battle)(void);

    /* get_day: the current scenario day (= CScenarioInfo.currentTurn), or -1 if unavailable (no
     *   in-process server / not in a game). Backs the timer's Timetable (per-day duration). Same
     *   struct_size guard applies. */
    int(__cdecl* get_day)(void);

    /* ---- timer host game-event layer (off[] keystone), appended after get_day (struct_size-guarded) --
     * turn_active:   1 when it is the LOCAL human's turn AND the End-Turn button exists (legacy ACTIVE).
     * is_animating:  1 while a battle attack animation is playing (BTN_DEFEND hidden) -> Animation Pause.
     * battle_kind:   0 none / 1 PvP (human-vs-human) / 2 any combat -> Combat Pause PvP vs PvAny.
     * turn_player_id: the current turn's player identity byte, or -1 (keys the plugin's extra-time bank).
     * retreat/end_day: on-elapse ACTIONS (press the captured game buttons); no-op + return 0 unless the
     *   host's action gate is enabled. Return 1 when a press was issued. */
    int(__cdecl* turn_active)(void);
    int(__cdecl* is_animating)(void);
    int(__cdecl* battle_kind)(void);
    int(__cdecl* turn_player_id)(void);
    int(__cdecl* retreat)(void);
    int(__cdecl* end_day)(void);
} C4P_Host;

/* What the plugin reports at query time (before init). */
typedef struct C4P_Info
{
    uint32_t struct_size;  /* sizeof(C4P_Info) */
    uint32_t abi_version;  /* must equal C4P_ABI_VERSION */
    const char* id;        /* stable unique id (used for dedup) */
    const char* name;      /* human-readable display name */
    /* Optional (ABI v2): the 12-byte GetId of a legacy .mod this plugin replaces. When both this
     * .c4p and that legacy .mod are present, the host loads this one and drops the legacy (so the
     * timer is not drawn twice). NULL = does not supersede anything. */
    const uint8_t* supersedes_legacy_id;
} C4P_Info;

/* ---- the exports a .c4p plugin must provide (extern "C", __cdecl) ---- */

/* Fill *out (abi_version, id, name). Return 1 on success. Called first; if abi_version mismatches
 * or it returns 0, the plugin is unloaded. */
int __cdecl c4p_query(C4P_Info* out);

/* One-time init. host is valid for the plugin's whole lifetime. Return 1 on success. */
int __cdecl c4p_init(const C4P_Host* host);

/* Cheap per-frame logic update (no drawing). now_ms is a GetTickCount-style millisecond clock.
 * Call host->invalidate() from here (or anywhere) when the overlay needs to be redrawn. */
void __cdecl c4p_tick(uint32_t now_ms);

/* Draw the overlay onto canvas (already cleared to transparent). Called only when the plugin is
 * dirty (after invalidate) or on the first frame. Return 1 if anything was drawn. */
int __cdecl c4p_draw(C4P_Canvas* canvas);

/* Optional teardown at unload (may be absent). */
void __cdecl c4p_shutdown(void);

/* Optional (may be absent): build the plugin's configuration submenu using WM_COMMAND ids starting
 * at base_cmd_id (the host reserves a 0x100-wide id block per plugin) and return the popup HMENU.
 * The host grafts it under its "Plugins" menu. Keep the HMENU to update your own check/radio marks
 * from c4p_command. Return NULL for no menu. */
HMENU __cdecl c4p_menu(int base_cmd_id);

/* Optional: handle a menu WM_COMMAND whose id is in this plugin's reserved block
 * (base_cmd_id .. base_cmd_id + 0xFF). Called on the game UI thread. */
void __cdecl c4p_command(int cmd);

#ifdef __cplusplus
}
#endif

#endif /* C4PLUGIN_H */
