"""Compile actual map drag handlers with fake game/capture endpoints (Windows/MSVC)."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'c4ddraw/features/featuremenu.cpp').read_text(encoding='utf-8-sig')
run = Path(tempfile.mkdtemp(prefix='map-drag-', dir=str(root / '.diagnostics')))

def function(name):
    found = re.findall(r'(?ms)^(?:bool|void|int __fastcall) ' + name +
                       r'\([^;{}]*\)\s*\{.*?^\}', source)
    assert len(found) == 1, name
    return found[0]

code = r'''
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
struct PointI { int x, y; };
bool g_dragScroll = true, g_dragScrollActive = false, g_dragMoved = false;
UINT g_dragScrollButton = 0;
PointI g_dragStart{}, g_dragMapCenter{}, g_dragPointerAnchor{};
const int kDragStartThreshold = 1;
HWND g_gameHwnd = (HWND)1, capture = nullptr;
int nativeCalls = 0, lastMessage = 0, pans = 0, checks = 0;
bool overMap = true;
#define CHECK(c) do { ++checks; if (!(c)) { printf("FAIL %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define GetCapture fakeGetCapture
#define SetCapture fakeSetCapture
#define ReleaseCapture fakeReleaseCapture
HWND fakeGetCapture() { return capture; }
void fakeSetCapture(HWND window) { capture = window; }
void fakeReleaseCapture() { capture = nullptr; }
void* mapGraphicsPtr() { return (void*)1; }
int testScreenToMap(void*, PointI*, PointI*, PointI*) { return overMap; }
void testGetMapCenter(void*, PointI* p, int* x, int* y) { *p = {5, 6}; *x = 2; *y = 3; }
void panMapCenterSmooth(void*, PointI*, int, int) { ++pans; }
void normalizeDragBoundary(void*, int, int) {}
int callOrigIsoMouse(void*, int msg, PointI*) { ++nativeCalls; lastMessage = msg; return 17; }
'''
for name in ('dragScrollReleaseMatches', 'dragScrollButtonHeld', 'cancelDragScroll',
             'dragThresholdExceeded', 'isoMouseHook'):
    code += function(name).replace('reinterpret_cast<ScreenToMapFn>(0x5418BA)', 'testScreenToMap').replace(
        'reinterpret_cast<GetMapCenterFn>(0x5414BC)', 'testGetMapCenter') + '\n'
code += r'''
int main() {
    PointI point{100, 100};
    isoMouseHook(nullptr, nullptr, WM_MBUTTONDOWN, &point);
    CHECK(g_dragScrollActive && g_dragScrollButton == WM_MBUTTONDOWN && capture == g_gameHwnd);
    CHECK(dragScrollButtonHeld(MK_MBUTTON) && !dragScrollButtonHeld(MK_LBUTTON));
    CHECK(!dragScrollReleaseMatches(WM_LBUTTONUP) && dragScrollReleaseMatches(WM_MBUTTONUP));
    isoMouseHook(nullptr, nullptr, WM_LBUTTONDOWN, &point);
    isoMouseHook(nullptr, nullptr, WM_LBUTTONUP, &point);
    CHECK(g_dragScrollButton == WM_MBUTTONDOWN && nativeCalls == 0);
    isoMouseHook(nullptr, nullptr, WM_MBUTTONUP, &point);
    CHECK(!g_dragScrollActive && !capture && !g_dragScrollButton && nativeCalls == 0);
    isoMouseHook(nullptr, nullptr, WM_MBUTTONDOWN, &point);
    ++point.x;
    isoMouseHook(nullptr, nullptr, WM_MOUSEMOVE, &point);
    CHECK(g_dragMoved && pans == 1);
    isoMouseHook(nullptr, nullptr, WM_MBUTTONUP, &point);
    CHECK(nativeCalls == 0 && !capture);
    isoMouseHook(nullptr, nullptr, WM_LBUTTONDOWN, &point);
    isoMouseHook(nullptr, nullptr, WM_LBUTTONUP, &point);
    CHECK(nativeCalls == 1 && lastMessage == WM_LBUTTONDOWN); // no stale-view UP
    isoMouseHook(nullptr, nullptr, WM_LBUTTONDOWN, &point);
    CHECK(dragScrollButtonHeld(MK_LBUTTON) && !dragScrollButtonHeld(MK_MBUTTON));
    isoMouseHook(nullptr, nullptr, WM_MBUTTONDOWN, &point);
    isoMouseHook(nullptr, nullptr, WM_MBUTTONUP, &point);
    CHECK(g_dragScrollButton == WM_LBUTTONDOWN && nativeCalls == 1);
    cancelDragScroll(); // focus/capture loss and toggle OFF share this cleanup
    CHECK(!capture && !g_dragScrollActive && !g_dragScrollButton && !dragScrollButtonHeld(MK_MBUTTON));
    g_dragScroll = false;
    CHECK(isoMouseHook(nullptr, nullptr, WM_MBUTTONDOWN, &point) == 17 && !g_dragScrollActive);
    g_dragScroll = true; overMap = false;
    CHECK(isoMouseHook(nullptr, nullptr, WM_MBUTTONDOWN, &point) == 17 && !g_dragScrollActive);
    printf("PASS: %d actual map-drag handler checks\n", checks);
}
'''
(run / 'map_drag.cpp').write_text(code, encoding='utf-8')
project = (root / 'c4ddraw/tests/filter_defaults_tests.vcxproj').read_text()
project = project.replace('filter_defaults_tests.c', 'map_drag.cpp').replace('filter_defaults_tests', 'map_drag')
(run / 'map_drag.vcxproj').write_text(project, encoding='utf-8')
vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
msbuild = subprocess.check_output([str(vswhere), '-latest', '-products', '*', '-requires',
    'Microsoft.Component.MSBuild', '-find', r'MSBuild\**\Bin\MSBuild.exe'], text=True).strip().splitlines()[0]
subprocess.run([msbuild, str(run / 'map_drag.vcxproj'), '/t:Build', '/p:Configuration=Release',
    '/p:Platform=Win32', '/p:FilterExtractDir=' + str(run), '/nologo', '/v:minimal'], check=True, env={k.upper(): v for k, v in os.environ.items()})
subprocess.run([str(run / 'bin/map_drag.exe')], check=True)
print('Evidence:', run)
