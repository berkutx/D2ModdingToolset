/*
 * C4dll-R plugin API v2 (.c4p): BGRA32 overlay DLLs in <game>\mods\, composited over the 8bpp game frame.
 * c4p_draw runs only when the plugin marks itself dirty (host->invalidate); the cached overlay is composited every frame.
 * Turn detection is host-driven so plugins stay portable. Only native .c4p plugins are loaded.
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

/* Overlay surface: BGRA8888 (32bppARGB), top-down, client-sized, STRAIGHT alpha (host premultiplies).
 * Pre-cleared to transparent before each c4p_draw. */
typedef struct C4P_Canvas
{
    uint32_t struct_size; /* sizeof(C4P_Canvas) */
    void* pixels;         /* BGRA32 top-down */
    int32_t width;
    int32_t height;
    int32_t stride; /* bytes per row */
} C4P_Canvas;

/* Host services, valid for the plugin's lifetime. */
typedef struct C4P_Host
{
    uint32_t struct_size; /* sizeof(C4P_Host) */
    HWND(__cdecl* get_hwnd)(void);
    void(__cdecl* invalidate)(void); /* request c4p_draw on the next frame */
    int(__cdecl* get_config_int)(const char* section, const char* key, int def);
    void(__cdecl* set_config_int)(const char* section, const char* key, int value);
    const char*(__cdecl* config_path)(void); /* shared C4plugins.ini next to the exe */

    /* turn state (ABI v2):
     * get_turn_serial: bumps on every turn change (incl. skipped turn); reset countdowns when it changes.
     * get_turn_player: current turn player index (>=0), or -1 if unknown / not in game.
     * is_in_game: 1 while a scenario turn is in progress. */
    uint32_t(__cdecl* get_turn_serial)(void);
    int(__cdecl* get_turn_player)(void);
    int(__cdecl* is_in_game)(void);

    /* Appended after ABI v2, no version bump: callers MUST check struct_size >= sizeof(C4P_Host) before calling.
     * is_in_battle: 1 while a battle viewer is on screen (Combat Pause). */
    int(__cdecl* is_in_battle)(void);

    /* get_day: current scenario day (CScenarioInfo.currentTurn), or -1 if unavailable. struct_size guard applies. */
    int(__cdecl* get_day)(void);

    /* Timer host game-event layer (off[] keystone), appended after get_day (struct_size-guarded):
     * turn_active:   1 when local human's turn AND End-Turn button exists.
     * is_animating:  1 while a battle attack animation plays (Animation Pause).
     * battle_kind:   0 none / 1 PvP / 2 any combat (Combat Pause PvP vs PvAny).
     * turn_player_id: current turn player identity byte, or -1.
     * retreat/end_day: on-elapse actions - queue the press (host performs it on its idle WM_TIMER); return 1.
     * cancel_elapse: discard queued actions when the clock becomes positive or its turn changes.
     * begin_turn_ack_serial: Russobit-only monotonic counter bumped after BTN_OK's normal
     *   DLG_BEGIN_TURN callback finishes; UINT32_MAX means that this executable has no hook. */
    int(__cdecl* turn_active)(void);
    int(__cdecl* is_animating)(void);
    int(__cdecl* battle_kind)(void);
    int(__cdecl* turn_player_id)(void);
    int(__cdecl* retreat)(void);
    int(__cdecl* end_day)(void);
    int(__cdecl* cancel_elapse)(void);
    uint32_t(__cdecl* begin_turn_ack_serial)(void);
} C4P_Host;

/* Plugin self-report, queried before init. */
typedef struct C4P_Info
{
    uint32_t struct_size;  /* sizeof(C4P_Info) */
    uint32_t abi_version;  /* must equal C4P_ABI_VERSION */
    const char* id;        /* stable unique id (dedup) */
    const char* name;      /* display name */
    /* Reserved ABI-v2 pointer slot. Ignored by the host; keep NULL. */
    const void* reserved_v2;
} C4P_Info;

/* ---- exports a .c4p must provide (extern "C", __cdecl) ---- */

/* Fill *out; return 1 on success. Called first; abi_version mismatch or 0 -> unloaded. */
int __cdecl c4p_query(C4P_Info* out);

/* One-time init; host valid for the plugin's lifetime. Return 1 on success. */
int __cdecl c4p_init(const C4P_Host* host);

/* Cheap per-frame update (no drawing). now_ms is a GetTickCount-style clock. Call host->invalidate() when a redraw is needed. */
void __cdecl c4p_tick(uint32_t now_ms);

/* Draw the overlay (canvas pre-cleared). Called only when dirty or on the first frame. Return 1 if anything was drawn. */
int __cdecl c4p_draw(C4P_Canvas* canvas);

/* Optional teardown at unload (may be absent). */
void __cdecl c4p_shutdown(void);

/* Optional: build the config submenu using WM_COMMAND ids from base_cmd_id (host reserves a 0x100-wide block per plugin);
 * return the popup HMENU (grafted under "Plugins"). Keep it to update your own marks from c4p_command. NULL = no menu. */
HMENU __cdecl c4p_menu(int base_cmd_id);

/* Optional: handle a WM_COMMAND in this plugin's block (base_cmd_id .. base_cmd_id+0xFF). Runs on the game UI thread. */
void __cdecl c4p_command(int cmd);

/* Optional: receive selected game-window input while the overlay remains click-through. Coordinates
 * are physical client pixels, matching C4P_Canvas. Return non-zero to consume the message. */
int __cdecl c4p_mouse(UINT msg, WPARAM wparam, int x, int y);

#ifdef __cplusplus
}
#endif

#endif /* C4PLUGIN_H */
