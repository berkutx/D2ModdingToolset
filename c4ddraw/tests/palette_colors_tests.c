/* Links the production ddpalette.c and checks the D2 8-bit -> RGB565 path. */
#include <stdio.h>
#include <string.h>
#include "dd.h"
#include "ddpalette.h"

CNCDDRAW g_ddraw;
static ULONG WINAPI test_add_ref(IDirectDrawPaletteImpl* palette) { return ++palette->ref; }
struct IDirectDrawPaletteImplVtbl g_ddp_vtbl = { NULL, test_add_ref };
void dbg_dump_ddp_flags(DWORD flags) { (void)flags; }
static int failures, checks;

static void check(int condition, const char* name)
{
    ++checks;
    if (!condition) ++failures;
    printf("%s %s\n", condition ? "PASS" : "FAIL", name);
}

static IDirectDrawPaletteImpl* create_palette(DWORD screen_bpp, DWORD flags, PALETTEENTRY* colors)
{
    IDirectDrawPaletteImpl* palette = NULL;
    g_ddraw.bpp = screen_bpp;
    if (dd_CreatePalette(flags, colors, &palette, NULL) != DD_OK || !palette) {
        fputs("Palette creation failed\n", stderr);
        ExitProcess(2);
    }
    return palette;
}

static DWORD rgb(const PALETTEENTRY* color)
{
    return ((DWORD)color->peRed << 16) | ((DWORD)color->peGreen << 8) | color->peBlue;
}

static int converted_pixels_equal(IDirectDrawPaletteImpl* palette, WORD expected)
{
    struct { BITMAPINFOHEADER header; RGBQUAD colors[256]; } source_info = { 0 };
    struct { BITMAPINFOHEADER header; DWORD masks[3]; } target_info = { 0 };
    BYTE* source_pixels = NULL;
    WORD* target_pixels = NULL;
    HDC source_dc = CreateCompatibleDC(NULL), target_dc = CreateCompatibleDC(NULL);
    HBITMAP source, target;
    HGDIOBJ old_source, old_target;
    int correct = 0, blit_ok;
    source_info.header.biSize = sizeof(BITMAPINFOHEADER);
    source_info.header.biWidth = 320;
    source_info.header.biHeight = -320;
    source_info.header.biPlanes = 1;
    source_info.header.biBitCount = 8;
    source_info.header.biClrUsed = 256;
    memcpy(source_info.colors, palette->data_rgb, sizeof(source_info.colors));
    target_info.header = source_info.header;
    target_info.header.biBitCount = 16;
    target_info.header.biClrUsed = 0;
    target_info.header.biCompression = BI_BITFIELDS;
    target_info.masks[0] = 0xf800;
    target_info.masks[1] = 0x07e0;
    target_info.masks[2] = 0x001f;
    source = CreateDIBSection(source_dc, (BITMAPINFO*)&source_info, DIB_RGB_COLORS,
                              (void**)&source_pixels, NULL, 0);
    target = CreateDIBSection(target_dc, (BITMAPINFO*)&target_info, DIB_RGB_COLORS,
                              (void**)&target_pixels, NULL, 0);
    if (!source_dc || !target_dc || !source || !target) ExitProcess(2);
    old_source = SelectObject(source_dc, source);
    old_target = SelectObject(target_dc, target);
    /* IsoAnim.ff record 1233 is exactly 320*320 pixels of palette index 255. */
    memset(source_pixels, 255, 320 * 320);
    memset(target_pixels, 0x55, 320 * 320 * sizeof(WORD));
    blit_ok = StretchBlt(target_dc, 0, 0, 320, 320, source_dc, 0, 0, 320, 320, SRCCOPY);
    GdiFlush();
    if (blit_ok) for (int i = 0; i < 320 * 320; ++i) correct += target_pixels[i] == expected;
    printf("  conversion: %d/102400 pixels = %04x, first = %04x\n", correct, expected, target_pixels[0]);
    SelectObject(source_dc, old_source);
    SelectObject(target_dc, old_target);
    DeleteObject(source); DeleteObject(target);
    DeleteDC(source_dc); DeleteDC(target_dc);
    return correct == 320 * 320;
}

int main(void)
{
    PALETTEENTRY colors[256] = { 0 }, result[256];
    IDirectDrawPaletteImpl* palette;
    colors[0].peRed = 27; colors[0].peBlue = 93;
    colors[1].peGreen = 132;
    colors[255].peRed = 255; colors[255].peBlue = 255;
    palette = create_palette(16, DDPCAPS_8BIT, colors);
    ddp_GetEntries(palette, 0, 0, 256, result);
    check(rgb(&result[0]) == rgb(&colors[0]), "16-bit canvas preserves image color 0");
    check(rgb(&result[1]) == rgb(&colors[1]), "ordinary image colors preserved");
    check(rgb(&result[255]) == rgb(&colors[255]), "16-bit canvas preserves transparent color 255");
    check(converted_pixels_equal(palette, 0xf81f), "index 255 converts to magenta color key");
    colors[255].peRed = 0; colors[255].peBlue = 0;
    ddp_SetEntries(palette, 0, 255, 1, colors + 255);
    ddp_GetEntries(palette, 0, 255, 1, result + 255);
    check(rgb(&result[255]) == 0, "partial palette update preserves black entry 255");
    HeapFree(GetProcessHeap(), 0, palette);

    memset(colors, 0, sizeof(colors));
    palette = create_palette(16, DDPCAPS_8BIT, colors);
    check(converted_pixels_equal(palette, 0), "empty city effect stays black for additive blending");
    HeapFree(GetProcessHeap(), 0, palette);

    colors[0].peRed = 61; colors[255].peGreen = 92;
    palette = create_palette(8, DDPCAPS_8BIT, colors);
    ddp_GetEntries(palette, 0, 0, 256, result);
    check(rgb(&result[0]) == 0 && rgb(&result[255]) == 0xffffff,
          "8-bit primary mode retains reserved black and white");
    HeapFree(GetProcessHeap(), 0, palette);
    palette = create_palette(8, DDPCAPS_8BIT | DDPCAPS_ALLOW256, colors);
    ddp_GetEntries(palette, 0, 0, 256, result);
    check(rgb(&result[0]) == rgb(&colors[0]) && rgb(&result[255]) == rgb(&colors[255]),
          "explicit ALLOW256 still preserves all colors");
    HeapFree(GetProcessHeap(), 0, palette);
    palette = create_palette(16, DDPCAPS_8BIT | DDPCAPS_PRIMARYSURFACE, colors);
    ddp_GetEntries(palette, 0, 0, 256, result);
    check(rgb(&result[0]) == 0 && rgb(&result[255]) == 0xffffff,
          "explicit primary palette retains requested reservation");
    HeapFree(GetProcessHeap(), 0, palette);
    check(dd_CreatePalette(DDPCAPS_8BIT, NULL, &palette, NULL) == DDERR_INVALIDPARAMS,
          "invalid palette input remains rejected");
    printf("%d checks, %d failures\n", checks, failures);
    return failures != 0;
}
