/* Actual upstream INI parser + maintained-patch cfg_get_string/cfg_get_int.
 * Fixtures are isolated files in the runner's newly created case directory.
 * The selected profile is supplied explicitly; profile discovery and rendering are not mocked as tested. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "filter-defaults-extracted.h"

static int failures;
static int cases;
static const char* lanczos = "Shaders\\interpolation\\lanczos2-sharp.glsl";
static const char* bicubic = "Shaders\\interpolation\\catmull-rom-bilinear.glsl";
static const char* nearest = "Shaders\\nearest-neighbor.glsl";

static void check(const char* name, const char* text, const char* profile,
                  const char* expectedShader, int expectedPortable)
{
    char relative[80], path[MAX_PATH], after[2048] = {0};
    FILE* file;
    size_t length;
    ++cases;
    sprintf(relative, "filter-fixture-%02d.ini", cases);
    if (!GetFullPathNameA(relative, sizeof(path), path, NULL)) exit(2);
    file = fopen(path, "wb");
    if (!file || fwrite(text, 1, strlen(text), file) != strlen(text)) exit(2);
    fclose(file);
    memset(&g_config, 0, sizeof(g_config));
    strcpy(g_config.game_section, profile);
    ini_create(&g_config.ini, path);
    load_filter_defaults();
    if (strcmp(g_config.shader, expectedShader) || g_config.d3d9_filter != expectedPortable) {
        printf("FAIL %s: shader=[%s] portable=%d\n", name, g_config.shader, g_config.d3d9_filter);
        ++failures;
    } else {
        printf("PASS %s\n", name);
    }
    ini_free(&g_config.ini);
    file = fopen(path, "rb");
    if (!file) exit(2);
    length = fread(after, 1, sizeof(after)-1, file);
    fclose(file);
    if (length != strlen(text) || memcmp(after, text, length)) {
        printf("FAIL %s: input INI was modified\n", name);
        ++failures;
    }
}

int main(void)
{
    check("missing keys", "[ddraw]\r\nwidth=0\r\n", "", lanczos, 3);
    check("explicit None", "[ddraw]\r\nshader=Shaders\\nearest-neighbor.glsl\r\nd3d9_filter=0\r\n", "", nearest, 0);
    check("explicit Bicubic", "[ddraw]\r\nshader=Shaders\\interpolation\\catmull-rom-bilinear.glsl\r\nd3d9_filter=2\r\n", "", bicubic, 2);
    check("profile override", "[ddraw]\r\nshader=Shaders\\interpolation\\lanczos2-sharp.glsl\r\nd3d9_filter=3\r\n[Discipl2]\r\nshader=Shaders\\nearest-neighbor.glsl\r\nd3d9_filter=0\r\n", "Discipl2", nearest, 0);
    check("profile inherits explicit values", "[ddraw]\r\nshader=Shaders\\interpolation\\catmull-rom-bilinear.glsl\r\nd3d9_filter=2\r\n[Discipl2/2]\r\nwidth=1000\r\n", "Discipl2/2", bicubic, 2);
    check("profile and global missing", "[ddraw]\r\nwidth=0\r\n[Discipl2/2]\r\nheight=0\r\n", "Discipl2/2", lanczos, 3);
    check("explicit global empty preserved", "[ddraw]\r\nshader=\r\nd3d9_filter=\r\n", "", "", 0);
    check("empty profile still inherits", "[ddraw]\r\nshader=Shaders\\interpolation\\catmull-rom-bilinear.glsl\r\nd3d9_filter=2\r\n[Discipl2]\r\nshader=\r\nd3d9_filter=\r\n", "Discipl2", bicubic, 2);
    check("custom shader preserved", "[ddraw]\r\nshader=MyShaders\\custom.glsl\r\n", "", "MyShaders\\custom.glsl", 3);
    check("explicit portable without shader", "[ddraw]\r\nd3d9_filter=0\r\n", "", lanczos, 0);
    check("invalid explicit integer unchanged", "[ddraw]\r\nd3d9_filter=99\r\n", "", lanczos, 99);
    printf("%d cases; %d failures; fixtures retained; no input INI writes by parser\n", cases, failures);
    return failures ? 1 : 0;
}
