#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include "../features/workarealayout.h"
static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %d: %s\n",__LINE__,#c); } } while(0)
static struct { BOOL windowed,fullscreen,window_workarea; RECT window_rect; } g_config;
static struct { HWND hwnd; struct { int width,height; } render; } g_ddraw;
static bool secondAvailable=true, minimized=false;
static char storedDevice[CCHDEVICENAME]="";
static LONG fakeStyle=WS_OVERLAPPEDWINDOW|WS_VISIBLE,fakeExstyle;
static RECT windowRect={100,100,900,700},normalRect=windowRect,clientRect={0,0,784,542};
static POINT clientOrigin={108,150};
static int writes, maxCalls, restoreCalls,relayouts;
static RECT work(int i) { RECT r=i==2?RECT{-1280,40,0,1024}:RECT{0,0,1920,1040}; return r; }
static HMONITOR mon(int i) { return reinterpret_cast<HMONITOR>(static_cast<INT_PTR>(i)); }
static int monitorNumber(HMONITOR m) { return static_cast<int>(reinterpret_cast<INT_PTR>(m)); }
static BOOL test_GetMonitorInfoA(HMONITOR m,MONITORINFO* info) {
    int i=monitorNumber(m); if(i<1||i>2||(i==2&&!secondAvailable)) return FALSE;
    auto* ex=reinterpret_cast<MONITORINFOEXA*>(info);
    info->rcWork=work(i); info->rcMonitor=i==2?RECT{-1280,0,0,1024}:RECT{0,0,1920,1080};
    lstrcpyA(ex->szDevice,i==2?"SECONDARY":"PRIMARY"); return TRUE;
}
static HMONITOR test_MonitorFromPoint(POINT p,DWORD flags) {
    return mon(flags==MONITOR_DEFAULTTOPRIMARY?1:(p.x<0&&secondAvailable?2:1));
}
static HMONITOR test_MonitorFromWindow(HWND,DWORD) {
    POINT p={(windowRect.left+windowRect.right)/2,(windowRect.top+windowRect.bottom)/2};
    return test_MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST);
}
static BOOL test_EnumDisplayMonitors(HDC,RECT*,MONITORENUMPROC cb,LPARAM ctx) {
    if(!cb(mon(1),nullptr,nullptr,ctx)) return TRUE;
    if(secondAvailable) cb(mon(2),nullptr,nullptr,ctx); return TRUE;
}
static BOOL test_AdjustWindowRectEx(RECT* r,DWORD,BOOL,DWORD) {
    r->left-=8; r->top-=50; r->right+=8; r->bottom+=8; return TRUE;
}
#define GetMonitorInfoA test_GetMonitorInfoA
#define MonitorFromPoint test_MonitorFromPoint
#define MonitorFromWindow test_MonitorFromWindow
#define EnumDisplayMonitors test_EnumDisplayMonitors
#define AdjustWindowRectEx test_AdjustWindowRectEx
static int DDReadConfigString(const char*,const char*,char* out,unsigned cap) { lstrcpynA(out,storedDevice,cap); return 1; }
static int DDWriteConfigString(const char* key,const char* val) {
    CHECK(std::strcmp(key,"window_monitor")==0); lstrcpynA(storedDevice,val,sizeof(storedDevice)); ++writes; return 1;
}
static LONG real_GetWindowLongA(HWND,int key) { return key==GWL_STYLE?fakeStyle:fakeExstyle; }
static LONG real_SetWindowLongA(HWND,int key,LONG v) { LONG& dst=key==GWL_STYLE?fakeStyle:fakeExstyle; LONG old=dst; dst=v; return old; }
static BOOL real_GetWindowRect(HWND,RECT* r) { *r=windowRect; return TRUE; }
static BOOL real_GetClientRect(HWND,RECT* r) { *r=clientRect; return TRUE; }
static BOOL real_ClientToScreen(HWND,POINT* p) { p->x+=clientOrigin.x; p->y+=clientOrigin.y; return TRUE; }
static BOOL real_SetWindowPos(HWND,HWND,int x,int y,int w,int h,UINT flags) {
    if(flags&SWP_NOSIZE) { w=windowRect.right-windowRect.left; h=windowRect.bottom-windowRect.top; }
    if(flags&SWP_NOMOVE) { x=windowRect.left; y=windowRect.top; }
    windowRect=RECT{x,y,x+w,y+h}; return TRUE;
}
static BOOL real_ShowWindow(HWND,int command) {
    if(command==SW_MAXIMIZE) {
        ++maxCalls;
        if(!(fakeStyle&WS_MAXIMIZE)) normalRect=windowRect;
        RECT r=work(monitorNumber(test_MonitorFromWindow(nullptr,0)));
        windowRect=RECT{r.left-8,r.top-8,r.right+8,r.bottom+8};
        clientRect=RECT{0,0,r.right-r.left,r.bottom-r.top-42};
        clientOrigin=POINT{r.left,r.top+42}; fakeStyle|=WS_MAXIMIZE;
    } else if(command==SW_RESTORE) { ++restoreCalls; fakeStyle&=~WS_MAXIMIZE; windowRect=normalRect; }
    return TRUE;
}
static int util_is_minimized(HWND) { return minimized?1:0; }
static int DDGetDisplayMode(void) { return !g_config.windowed?2:g_config.fullscreen?1:g_config.window_workarea?3:0; }
static void DDRelayoutCurrentMode(void) { ++relayouts; }
static int activeWide=1, fixedMenu=0, stretchPercent=100, currentGameW=1366,currentGameH=768,outputW=1796,outputH=1010;
static bool g_boxing=false,g_maintas=true;
static char g_aspectRatio[16]={};
static int horplus_is_active(void) { return activeWide; }
static int DDIsWindowStretchActive(void) { return fixedMenu; }
static int DDGetWindowStretchPercent(void) { return stretchPercent; }
static bool customAspect(double*) { return false; }
void predictViewport(int,int,int,int,int*,int*,int*,int*);
static int DDGetScaleMetrics(int* gw,int* gh,int* ow,int* oh,int* x,int* y,int* vw,int* vh) {
    if(gw)*gw=currentGameW; if(gh)*gh=currentGameH; if(ow)*ow=outputW; if(oh)*oh=outputH;
    predictViewport(currentGameW,currentGameH,outputW,outputH,x,y,vw,vh); return 1;
}
#include "windowmonitor-extracted.h"

static void reset() {
    g_initial_monitor_resolved=FALSE; g_initial_window_placed=FALSE; g_initial_monitor=nullptr;
    g_workarea_native_maximized=FALSE; g_saved_monitor_device[0]=0;
    g_config.windowed=TRUE; g_config.fullscreen=FALSE; g_config.window_workarea=FALSE;
    g_config.window_rect=RECT{-32000,-32000,0,0}; g_ddraw.hwnd=(HWND)1;
    windowRect=RECT{100,100,900,700}; fakeStyle=WS_OVERLAPPEDWINDOW|WS_VISIBLE;
    writes=maxCalls=restoreCalls=relayouts=0; minimized=false; secondAvailable=true;
}
static void monitor_tests() {
    reset(); lstrcpyA(storedDevice,"SECONDARY");
    int w=0,h=0,cw=0,ch=0,native=-1;
    CHECK(DDGetAutomaticCanvasOutput(&w,&h)&&w==1264&&h==926);
    CHECK(adaptiveCanvasForOutput(w,h,&cw,&ch,&native)&&cw==1024&&ch==768&&native==1);
    RECT outer={100,100,900,700}; DDPlaceInitialWindowRect(&outer);
    CHECK(outer.left<0&&outer.right<=0&&outer.top>=40&&outer.bottom<=1024);
    windowRect=outer; DDCompleteInitialWindowPlacement();
    CHECK(DDRememberWindowMonitor()==1&&writes==0);
    windowRect=RECT{300,200,1300,900};
    CHECK(DDGetAutomaticCanvasOutput(&w,&h)&&w==1904&&h==982);
    CHECK(DDRememberWindowMonitor()==2&&writes==1&&std::strcmp(storedDevice,"PRIMARY")==0);
    CHECK(DDRememberWindowMonitor()==1&&writes==1);
    reset(); lstrcpyA(storedDevice,"SECONDARY"); secondAvailable=false;
    CHECK(DDGetAutomaticCanvasOutput(&w,&h)&&w==1904&&h==982);
    CHECK(adaptiveCanvasForOutput(w,h,&cw,&ch,&native)&&cw==1600&&ch==900&&native==-1);
    reset(); storedDevice[0]=0; g_config.window_rect.left=-900; g_config.window_rect.top=100;
    CHECK(DDGetAutomaticCanvasOutput(&w,&h)&&w==1264);
    reset(); lstrcpyA(storedDevice,"SECONDARY"); g_config.fullscreen=TRUE;
    CHECK(DDGetAutomaticCanvasOutput(&w,&h)&&w==1904);
}
static void native_window_tests() {
    reset(); lstrcpyA(storedDevice,"SECONDARY"); g_config.window_workarea=TRUE;
    RECT client={};
    CHECK(DDApplyWorkAreaWindow(&client));
    CHECK((fakeStyle&WS_MAXIMIZE)!=0&&maxCalls==1);
    CHECK(client.left==-1280&&client.right==0&&client.top==82&&client.bottom==1024);
    CHECK(DDApplyWorkAreaWindow(&client)&&maxCalls==1);
    DDCompleteInitialWindowPlacement();
    g_ddraw.render.width=1280; g_ddraw.render.height=942;
    DDRefreshWorkAreaWindow(); CHECK(relayouts==0); // outer borders outside rcWork must not cause a loop
    g_ddraw.render.height=900;
    DDRefreshWorkAreaWindow(); CHECK(relayouts==1);
    minimized=true; DDRefreshWorkAreaWindow(); CHECK(relayouts==1); minimized=false;
    g_config.window_workarea=FALSE;
    CHECK(!DDApplyWorkAreaWindow(&client)&&!(fakeStyle&WS_MAXIMIZE)&&restoreCalls==1);
}
static void recommendation_tests() {
    activeWide=1;
    CHECK(adaptiveResolutionNeedsRestart(1366,768,1600,900,-1));
    CHECK(!adaptiveResolutionNeedsRestart(1600,900,1600,900,-1));
    activeWide=0;
    CHECK(adaptiveResolutionNeedsRestart(1600,900,1600,900,-1));
    CHECK(!adaptiveResolutionNeedsRestart(1024,768,1024,768,1));
    CHECK(!adaptiveResolutionNeedsRestart(0,0,1600,900,-1));
    g_config.window_workarea=TRUE; g_config.fullscreen=FALSE;
    fixedMenu=0;
    CHECK(resolutionReducesVisibleScaling(1366,768,1600,900));
    CHECK(!resolutionReducesVisibleScaling(1600,900,1366,768));
    fixedMenu=1; stretchPercent=100;
    CHECK(!resolutionReducesVisibleScaling(1366,768,1600,900));
    stretchPercent=50;
    CHECK(resolutionReducesVisibleScaling(1366,768,1600,900));
    fixedMenu=0; currentGameW=800; currentGameH=600; outputW=1280; outputH=942;
    CHECK(resolutionReducesVisibleScaling(800,600,1024,768));
    int w=0,h=0,n=-1;
    CHECK(adaptiveCanvasForOutput(1280,1024,&w,&h,&n)&&w==1280&&h==1024&&n==2);
    CHECK(adaptiveCanvasForOutput(900,680,&w,&h,&n)&&w==800&&h==600&&n==0);
}
int main() {
    monitor_tests(); native_window_tests(); recommendation_tests();
    std::printf("%s: %d checks (monitor/restart selection, native maximize, useful resolution recommendations)\n",failures?"FAIL":"PASS",checks);
    return failures?1:0;
}
