#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

static int checks, failures;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
static struct { BOOL maintas, boxing; char aspect_ratio[16]; } g_config;
static struct {
    int width, height, child_window_exists;
    volatile LONG upscale_hack_active;
    struct {
        int width, height;
        volatile LONG live_resize_active;
        struct { int x, y, width, height; } viewport;
        volatile LONG clear_screen;
        HANDLE sem;
    } render;
} g_ddraw;
static volatile LONG g_c4_d2_cursor_ownership;
static int decor_width = 800, decor_height = 600, decor_active = 1;
static short control_key;
static int horplus_get_decor_layout(int* width, int* height, int* wide_battle) {
    if (width) *width = decor_width;
    if (height) *height = decor_height;
    if (wide_battle) *wide_battle = decor_width == 990;
    return decor_active;
}
static SHORT test_GetKeyState(int key) { return key == VK_CONTROL ? control_key : 0; }
static BOOL real_ScreenToClient(HWND hwnd, POINT* point) { (void)hwnd; (void)point; return TRUE; }
static BOOL test_ReleaseSemaphore(HANDLE sem, LONG count, LONG* previous) {
    (void)sem; (void)count; (void)previous; return TRUE;
}
static int DDGetScaleMetrics(int* gw,int* gh,int* ow,int* oh,int* x,int* y,int* w,int* h) {
    if(gw)*gw=g_ddraw.width; if(gh)*gh=g_ddraw.height;
    if(ow)*ow=g_ddraw.render.width; if(oh)*oh=g_ddraw.render.height;
    if(x)*x=g_ddraw.render.viewport.x; if(y)*y=g_ddraw.render.viewport.y;
    if(w)*w=g_ddraw.render.viewport.width; if(h)*h=g_ddraw.render.viewport.height;
    return 1;
}
#define GetKeyState test_GetKeyState
#define ReleaseSemaphore test_ReleaseSemaphore
#include "windowstretch-extracted.h"

static void reset(void) {
    ZeroMemory(&g_config, sizeof(g_config));
    ZeroMemory(&g_ddraw, sizeof(g_ddraw));
    g_config.maintas = TRUE;
    g_c4_d2_cursor_ownership = 1;
    g_ddraw.width = 1366; g_ddraw.height = 768;
    g_ddraw.render.width = 1796; g_ddraw.render.height = 1026;
    g_ddraw.render.viewport.x = 0; g_ddraw.render.viewport.y = 8;
    g_ddraw.render.viewport.width = 1796; g_ddraw.render.viewport.height = 1010;
    decor_active = 1; decor_width = 800; decor_height = 600; control_key = 0;
    DDSetWindowStretchPercent(100);
}
static int correct(int bottom, int* x, int* y, int* w, int* h) {
    *x = g_ddraw.render.viewport.x;
    *y = bottom ? g_ddraw.render.height - g_ddraw.render.viewport.y - g_ddraw.render.viewport.height : g_ddraw.render.viewport.y;
    *w = g_ddraw.render.viewport.width; *h = g_ddraw.render.viewport.height;
    return DDApplyWindowStretchViewport(bottom, x, y, w, h);
}
static void unchanged(void) {
    int x,y,w,h;
    CHECK(!correct(0,&x,&y,&w,&h));
    CHECK(x==g_ddraw.render.viewport.x && y==g_ddraw.render.viewport.y &&
        w==g_ddraw.render.viewport.width && h==g_ddraw.render.viewport.height);
}
static void screenshot_geometry(void) {
    int x,y,w,h,xb,yb,wb,hb;
    double source_scale_x, source_scale_y;
    int cropw,croph;
    reset();
    CHECK(correct(0,&x,&y,&w,&h));
    CHECK(x==-14 && y==0 && w==1824 && h==1026);
    CHECK(fabs((double)w/1796.0-(double)h/1010.0)<1.0/1010.0);
    DDGetWindowStretchCrop(g_ddraw.width,g_ddraw.height,NULL,NULL,&cropw,&croph);
    source_scale_x=(double)w/cropw; source_scale_y=(double)h/croph;
    CHECK(fabs(source_scale_x-source_scale_y)<0.003);
    CHECK((double)x+(w-source_scale_x*800.0)/2.0>=0.0);
    CHECK((double)x+(w+source_scale_x*800.0)/2.0<=1796.0);
    decor_width=990;
    CHECK(correct(0,&x,&y,&w,&h));
    CHECK((double)x+(w-(double)w*990.0/cropw)/2.0>=0.0);
    CHECK((double)x+(w+(double)w*990.0/cropw)/2.0<=1796.0);
    CHECK(correct(1,&xb,&yb,&wb,&hb));
    CHECK(xb==x && wb==w && hb==h && yb==1026-y-h);
    g_ddraw.render.height=1027;
    CHECK(correct(0,&x,&y,&w,&h));
    CHECK(correct(1,&xb,&yb,&wb,&hb));
    CHECK(xb==x && wb==w && hb==h && yb==1027-y-h);
    reset();
    x=0; y=8; w=1796; h=1010;
    DDApplySimpleZoomViewport(0,&x,&y,&w,&h);
    CHECK(x==-14 && y==0 && w==1824 && h==1026);
}
static void gating_and_transitions(void) {
    int x,y,w,h,mx=17,my=23;
    reset(); decor_active=0; unchanged();
    CHECK(!DDMapWindowStretchMouse(700,0,&mx,&my) && mx==17 && my==23);
    decor_active=1; CHECK(correct(0,&x,&y,&w,&h));
    decor_active=0; unchanged();
    decor_active=1; CHECK(correct(0,&x,&y,&w,&h));
    reset(); DDSetWindowStretchPercent(0); unchanged();
    DDSetWindowStretchPercent(50); unchanged();
    DDSetWindowStretchPercent(99); unchanged();
    reset(); g_config.boxing=TRUE; unchanged();
    reset(); strcpy(g_config.aspect_ratio,"4:3"); unchanged();
    reset(); g_config.maintas=FALSE; unchanged();
    reset(); g_ddraw.child_window_exists=1; unchanged();
    reset(); g_ddraw.upscale_hack_active=1; unchanged();
    reset(); g_ddraw.render.live_resize_active=1; unchanged();
    reset(); g_c4_d2_cursor_ownership=0; unchanged();
    reset(); g_ddraw.render.width=1920; g_ddraw.render.height=1080;
    g_ddraw.render.viewport.width=1920; g_ddraw.render.viewport.height=1080; g_ddraw.render.viewport.y=0;
    unchanged();
    reset(); g_ddraw.render.height=1010; g_ddraw.render.viewport.y=0; unchanged();
}
static void narrow_output_protects_controls(void) {
    int x,y,w,h,cropw,croph,wide;
    for(wide=0;wide<2;++wide) {
        reset(); decor_width=wide?990:800;
        g_ddraw.render.width=800; g_ddraw.render.height=1000;
        g_ddraw.render.viewport.width=800; g_ddraw.render.viewport.height=450; g_ddraw.render.viewport.y=275;
        CHECK(correct(0,&x,&y,&w,&h));
        DDGetWindowStretchCrop(g_ddraw.width,g_ddraw.height,NULL,NULL,&cropw,&croph);
        CHECK((double)x+((double)w-(double)w*decor_width/cropw)/2.0>=-1.0);
        CHECK((double)x+((double)w+(double)w*decor_width/cropw)/2.0<=801.0);
        CHECK(y>=0 && y+h<=1000 && h<1000);
        CHECK(fabs((double)w/800.0-(double)h/450.0)<1.0/450.0);
    }
}
static void physical_and_normalized_mouse(void) {
    static const int positions[][2]={{0,0},{1795,0},{0,1025},{1795,1025},{898,513},{450,8},{1347,1017}};
    unsigned i;
    int x,y,w,h,mx,my,l,t,cw,ch;
    reset();
    correct(0,&x,&y,&w,&h);
    DDGetWindowStretchCrop(g_ddraw.width,g_ddraw.height,&l,&t,&cw,&ch);
    CHECK(DDMapWindowStretchMouse(898,0,&mx,&my));
    CHECK(abs(my-t)<=1);
    CHECK(DDMapWindowStretchMouse(898,1025,&mx,&my));
    CHECK(abs(my-(t+ch-1))<=1);
    for(i=0;i<sizeof(positions)/sizeof(positions[0]);++i) {
        int px=positions[i][0],py=positions[i][1];
        int nx=(int)((double)(px-g_ddraw.render.viewport.x)*g_ddraw.width/g_ddraw.render.viewport.width);
        int ny=(int)((double)(py-g_ddraw.render.viewport.y)*g_ddraw.height/g_ddraw.render.viewport.height);
        int expected_x=(int)(l+(double)(px-x)*cw/w);
        int expected_y=(int)(t+(double)(py-y)*ch/h);
        CHECK(DDMapWindowStretchMouse(px,py,&mx,&my));
        CHECK(abs(mx-expected_x)<=1 && abs(my-expected_y)<=1);
        DDApplySimpleZoomMouse(&nx,&ny,g_ddraw.width,g_ddraw.height);
        CHECK(abs(mx-nx)<=1 && abs(my-ny)<=1);
    }
}
static void extra_zoom_anchor(void) {
    int before_x,before_y,after_x,after_y,x,y,w,h;
    int anchor_x=700,anchor_y=300;
    reset();
    CHECK(DDMapWindowStretchMouse(anchor_x,anchor_y,&before_x,&before_y));
    control_key=-32768;
    CHECK(DDHandleSimpleZoom(NULL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(anchor_x,anchor_y)));
    CHECK(DDGetSimpleZoomExtra1000()==1100);
    CHECK(DDMapWindowStretchMouse(anchor_x,anchor_y,&after_x,&after_y));
    CHECK(abs(before_x-after_x)<=1 && abs(before_y-after_y)<=1);
    x=0; y=8; w=1796; h=1010;
    DDApplySimpleZoomViewport(0,&x,&y,&w,&h);
    CHECK(w>1824 && h>1026 && x<-14 && y<0);
    DDSetWindowStretchPercent(100);
    CHECK(DDGetSimpleZoomExtra1000()==1000);
    CHECK(DDMapWindowStretchMouse(anchor_x,anchor_y,&after_x,&after_y));
    CHECK(before_x==after_x && before_y==after_y);
}
static void repeated_application_preserves_capped_rect(void) {
    int x,y,w,h,first_x,first_y,first_w,first_h,direct_x,direct_y,direct_w,direct_h;
    reset();
    g_ddraw.render.width=813; g_ddraw.render.height=801;
    g_ddraw.render.viewport.width=813; g_ddraw.render.viewport.height=457;
    g_ddraw.render.viewport.y=(801-457)/2;
    CHECK(correct(0,&x,&y,&w,&h));
    first_x=x; first_y=y; first_w=w; first_h=h;
    DDApplyWindowStretchViewport(0,&x,&y,&w,&h);
    CHECK(x==first_x && y==first_y && w==first_w && h==first_h);
    control_key=-32768;
    DDHandleSimpleZoom(NULL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(400,300));
    direct_x=g_ddraw.render.viewport.x; direct_y=g_ddraw.render.viewport.y;
    direct_w=g_ddraw.render.viewport.width; direct_h=g_ddraw.render.viewport.height;
    DDApplySimpleZoomViewport(0,&direct_x,&direct_y,&direct_w,&direct_h);
    x=first_x; y=first_y; w=first_w; h=first_h;
    DDApplySimpleZoomViewport(0,&x,&y,&w,&h);
    CHECK(x==direct_x && y==direct_y && w==direct_w && h==direct_h);
}
static void odd_crop_preserves_exact_protected_bounds(void) {
    int x,y,w,h,crop_left,crop_top,crop_width,crop_height,content_left,content_top;
    double visible_left,visible_top,visible_right,visible_bottom;
    reset();
    g_ddraw.width=1600; g_ddraw.height=900;
    g_ddraw.render.width=1000; g_ddraw.render.height=801;
    g_ddraw.render.viewport.width=1000; g_ddraw.render.viewport.height=562;
    g_ddraw.render.viewport.y=(801-562)/2;
    CHECK(correct(0,&x,&y,&w,&h));
    DDGetWindowStretchCrop(g_ddraw.width,g_ddraw.height,&crop_left,&crop_top,&crop_width,&crop_height);
    CHECK(crop_width==1067 && crop_left==266);
    content_left=(g_ddraw.width-decor_width)/2;
    content_top=(g_ddraw.height-decor_height)/2;
    visible_left=x+(double)(content_left-crop_left)*w/crop_width;
    visible_right=x+(double)(content_left+decor_width-crop_left)*w/crop_width;
    visible_top=y+(double)(content_top-crop_top)*h/crop_height;
    visible_bottom=y+(double)(content_top+decor_height-crop_top)*h/crop_height;
    CHECK(visible_left>=0.0 && visible_right<=g_ddraw.render.width);
    CHECK(visible_top>=0.0 && visible_bottom<=g_ddraw.render.height);
}
static void minimum_canvas_fill_is_active(void) {
    int x,y,w,h;
    reset();
    g_ddraw.width=1066; g_ddraw.height=600;
    CHECK(!DDGetWindowStretchCrop(g_ddraw.width,g_ddraw.height,NULL,NULL,NULL,NULL));
    CHECK(correct(0,&x,&y,&w,&h));
    CHECK(DDIsWindowStretchActive());
    DDSetWindowStretchPercent(0);
    CHECK(!DDIsWindowStretchActive());
}
static void plugin_visible_rectangle(void) {
    int32_t l,t,w,h,zoom;
    int crop_left,crop_top,crop_width,crop_height;
    reset();
    DDGetWindowStretchCrop(g_ddraw.width,g_ddraw.height,&crop_left,&crop_top,&crop_width,&crop_height);
    CHECK(host_get_visible_game_rect(&l,&t,&w,&h,&zoom));
    CHECK(l==158 && t==84 && w==1050 && h==600);
    CHECK(l>crop_left && l+w<crop_left+crop_width);
    CHECK(t==crop_top && h==crop_height);
    CHECK(abs(zoom-DDGetSimpleZoom1000())<=1);
    control_key=-32768;
    DDHandleSimpleZoom(NULL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(700,300));
    CHECK(host_get_visible_game_rect(&l,&t,&w,&h,&zoom));
    CHECK(w<1050 && h<600 && zoom>1300);
    CHECK(l>=crop_left && t>=crop_top && l+w<=crop_left+crop_width && t+h<=crop_top+crop_height);
    CHECK(abs(zoom-DDGetSimpleZoom1000())<=1);
    reset(); decor_active=0;
    CHECK(host_get_visible_game_rect(&l,&t,&w,&h,&zoom));
    CHECK(l==0 && t==0 && w==1366 && h==768 && zoom==1000);
}
int main(void) {
    screenshot_geometry(); gating_and_transitions(); narrow_output_protects_controls();
    physical_and_normalized_mouse(); extra_zoom_anchor(); plugin_visible_rectangle();
    repeated_application_preserves_capped_rect(); odd_crop_preserves_exact_protected_bounds(); minimum_canvas_fill_is_active();
    printf("Window-stretch regression checks: %d passed, %d failed\n",checks-failures,failures);
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
