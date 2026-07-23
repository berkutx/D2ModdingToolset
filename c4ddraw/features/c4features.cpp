/*
 * C4dll-R feature bootstrap.
 *
 * cnc-ddraw only calls this one integration entry point. Individual feature lifetime and ordering
 * stay in our wrapper sources instead of leaking into the upstream patch.
 */

extern "C" void localization_install(void);
extern "C" void savelogic_install(void);
extern "C" void cursorfix_install(void);
extern "C" void featuremenu_install(void);
extern "C" void pluginhost_install(void);
extern "C" void headless_install(void);

extern "C" void c4features_install(void)
{
    localization_install();
    savelogic_install();
    cursorfix_install();
    featuremenu_install();
    pluginhost_install();
    headless_install();
}
