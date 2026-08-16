/*
 * C4dll-R feature bootstrap.
 *
 * cnc-ddraw only calls this one integration entry point. Individual feature lifetime and ordering
 * stay in our wrapper sources instead of leaking into the upstream patch.
 */

extern "C" void localization_install(void);
extern "C" void savelogic_install(void);
extern "C" void horplus_install(void);
extern "C" void widebattle_install(void);
extern "C" void decorative_install(void);
extern "C" void clouds_install(void);
extern "C" void featuremenu_install(void);
extern "C" void pluginhost_install(void);
extern "C" void headless_install(void);

extern "C" void c4features_install(void)
{
    localization_install();
    savelogic_install();
    // Signature-gated game hooks must publish their availability before the menu reads its config.
    widebattle_install();
    // Hor+ shares WideBattle's exact-build battle-centering hook, so install it second.
    horplus_install();
    decorative_install();
    clouds_install();
    featuremenu_install();
    pluginhost_install();
    headless_install();
}
