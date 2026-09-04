#pragma once

// Explicit reset, not a migration. Only wrapper-owned keys: never timer/plugin
// sections or native gameplay options. Other profiles and unknown keys survive.
namespace c4defaults {
struct Setting { const char* key; const char* value; };
static const Setting renderer[] = {
    {"renderer", "opengl"}, {"shader", "Shaders\\interpolation\\lanczos2-sharp.glsl"},
    {"d3d9_filter", "3"}, {"width", "0"}, {"height", "0"},
    {"posX", "-32000"}, {"posY", "-32000"},
    {"windowed", "true"}, {"fullscreen", "false"},
    {"border", "true"}, {"resizable", "true"}, {"savesettings", "1"},
    {"maintas", "true"}, {"boxing", "false"}, {"aspect_ratio", ""},
    {"toggle_borderless", "false"}, {"toggle_upscaled", "false"},
    {"maxfps", "-1"}, {"maxgameticks", "180"}, {"vsync", "false"},
    {"singlecpu", "true"}, {"adjmouse", "true"}, {"devmode", "true"},
    {"noactivateapp", "true"}, {"nonexclusive", "true"},
    {"fake_mode", "1024x768x16"}, {"resolutions", "0"}, {"fixchilds", "2"},
    {"keytogglefullscreen", "0x0D"}, {"keytogglemaximize", "0x22"},
    {"keyunlockcursor1", "0x09"}, {"keyunlockcursor2", "0xA3"},
    {"keyscreenshot", "0x2C"}
};
static const Setting menu[] = {
    {"language", "auto"}, {"alwaysActive", "0"},
    {"battleAnimEnabled", "1"}, {"battleAnimSpeed", "2"}, {"battleAnimFactor", "20"},
    {"mapAnimEnabled", "0"}, {"mapAnimSpeed", "2"}, {"mapAnimFactor", "10"},
    {"battleAttackEnabled", "1"}, {"battleAttackSpeed", "5"}, {"perUnitBurst", "0"},
    {"dragScroll", "1"}, {"wideBattle", "1"}, {"dialogVoSkip", "0"},
    {"fastAI", "0"}, {"stretchWindows", "100"}, {"messageBatching", "1"},
    {"debugLog", "0"}, {"netTrace", "0"}
};
static const Setting wrapper[] = {
    {"Archive", "1"}, {"IncludeSubdirectories", "0"}
};
}
