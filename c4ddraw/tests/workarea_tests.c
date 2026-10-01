#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "../features/workarealayout.h"

static int failures, checks;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static struct {
    BOOL windowed, fullscreen, window_workarea, resizable, toggle_borderless;
    BOOL boxing, maintas;
    RECT window_rect;
    char aspect_ratio[16];
} g_config;
static struct { int ref; HWND hwnd; int width, height; struct { int width, height; } render; } g_ddraw;
static WINDOWPLACEMENT g_last_normal_window_placement;
static LONG g_restore_normal_placement_pending;
static int captures, relayouts, restores, saved_workarea, write_ok = 1, guard_toggle;
int DDGetDisplayMode(void);
static void DDCaptureNormalWindowPlacement(void) {
    if (DDGetDisplayMode() == 0) {
        ++captures;
        g_last_normal_window_placement.length = sizeof(WINDOWPLACEMENT);
    }
}
static int DDWriteConfigString(const char* key, const char* value) {
    CHECK(strcmp(key, "window_workarea") == 0);
    if (write_ok) saved_workarea = strcmp(value, "true") == 0;
    return write_ok;
}
static void DDRelayoutCurrentMode(void) { ++relayouts; }
static void DDCompleteWindowedModeToggle(void) {
    if (InterlockedExchange(&g_restore_normal_placement_pending, 0)) ++restores;
}
static BOOL dd_prepare_normal_window_output(RECT* saved) { ZeroMemory(saved, sizeof(*saved)); return TRUE; }
static void dd_restore_output_request(const RECT* saved) { (void)saved; }
static void util_toggle_fullscreen(void) {
    if (guard_toggle) return;
    if (g_config.toggle_borderless) g_config.fullscreen = !g_config.fullscreen;
    else g_config.windowed = !g_config.windowed;
}
#include "workarea-extracted.h"

static void check_work(RECT work, RECT chrome) {
    RECT client = {0};
    CHECK(c4_workarea_client_rect(&work, &chrome, &client));
    CHECK(client.left + chrome.left == work.left);
    CHECK(client.top + chrome.top == work.top);
    CHECK(client.right + chrome.right == work.right);
    CHECK(client.bottom + chrome.bottom == work.bottom);
    CHECK(client.right > client.left && client.bottom > client.top);
}
static void geometry_tests(void) {
    RECT chrome = {-8, -50, 8, 8};
    RECT work[] = {
        {0,0,1920,1040}, {0,40,1920,1080}, {80,0,1920,1080},
        {0,0,1840,1080}, {-1920,0,0,1040}, {0,-1200,1920,-40},
        {1920,80,4480,1440}, {0,0,800,560}
    };
    unsigned i;
    for (i=0; i<sizeof(work)/sizeof(work[0]); ++i) check_work(work[i],chrome);
    chrome.left=-16; chrome.top=-100; chrome.right=16; chrome.bottom=16;
    check_work(work[0],chrome);
    {
        RECT tiny={0,0,20,20}, out={1,2,3,4};
        CHECK(!c4_workarea_client_rect(&tiny,&chrome,&out));
        CHECK(out.left==1 && out.top==2 && out.right==3 && out.bottom==4);
        CHECK(!c4_workarea_client_rect(NULL,&chrome,&out));
    }
}
static void viewport_tests(void) {
    int x,y,w,h;
    g_ddraw.width=1920; g_ddraw.height=1080;
    g_config.windowed=TRUE; g_config.fullscreen=FALSE; g_config.window_workarea=TRUE;
    g_config.maintas=TRUE;
    dd_CalcViewport(1904,980,&x,&y,&w,&h);
    CHECK(x==81 && y==0 && w==1742 && h==980);
    g_config.boxing=TRUE; g_config.maintas=FALSE;
    dd_CalcViewport(1904,980,&x,&y,&w,&h);
    CHECK(x==81 && y==0 && w==1742 && h==980);
    g_ddraw.width=800; g_ddraw.height=600;
    dd_CalcViewport(1904,982,&x,&y,&w,&h);
    CHECK(w==800 && h==600 && x==552 && y==191);
    dd_CalcViewport(2544,1340,&x,&y,&w,&h);
    CHECK(w==1600 && h==1200 && x==472 && y==70);
    g_config.boxing=FALSE;
    dd_CalcViewport(1904,982,&x,&y,&w,&h);
    CHECK(w==1904 && h==982 && x==0 && y==0);
}
static void transition_tests(void) {
    g_ddraw.ref=1; g_ddraw.hwnd=(HWND)1; g_ddraw.width=1024; g_ddraw.height=768;
    g_config.windowed=TRUE; g_config.fullscreen=FALSE; g_config.window_workarea=FALSE;
    g_config.resizable=FALSE; /* caption maximize must also work with fixed-size normal windows */
    CHECK(DDGetDisplayMode()==0);
    CHECK(DDToggleWorkAreaWindow()==1 && DDGetDisplayMode()==3 && saved_workarea==1);
    CHECK(captures==1 && relayouts==1);
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==1);
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==3 && captures==1);
    CHECK(DDToggleWorkAreaWindow()==1 && DDGetDisplayMode()==0 && saved_workarea==0);
    CHECK(restores==1);
    write_ok=0;
    CHECK(DDToggleWorkAreaWindow()==1 && DDGetDisplayMode()==0);
    write_ok=1;
    DDPrepareDisplayModeChange(3); CHECK(captures==2);
    g_config.window_workarea=TRUE;
    DDPrepareDisplayModeChange(0); CHECK(g_restore_normal_placement_pending==1);
    g_config.window_workarea=FALSE; DDCompleteWindowedModeToggle();
    CHECK(restores==2);
    g_config.window_workarea=TRUE;
    g_ddraw.render.width=1904; g_ddraw.render.height=982;
    DDLeaveWorkAreaForManualMove();
    CHECK(DDGetDisplayMode()==0 && saved_workarea==0);
    CHECK(g_config.window_rect.right==1904 && g_config.window_rect.bottom==982);
    g_config.window_workarea=TRUE; guard_toggle=1;
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==3);
    guard_toggle=0;
    g_config.windowed=FALSE; g_config.fullscreen=FALSE;
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==3);
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==2);
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==3);
    g_config.window_workarea=FALSE;
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==2);
    DDToggleWindowedMode(); CHECK(DDGetDisplayMode()==0);
}
int main(void) {
    geometry_tests(); viewport_tests(); transition_tests();
    printf("%s: %d checks (work-area geometry, scaling, window/fullscreen transitions)\n",
        failures ? "FAIL" : "PASS",checks);
    return failures ? 1 : 0;
}
