"""Source-contract guardrails for reset scope; not a live renderer/network integration test."""
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
FEATURES = ROOT / 'c4ddraw/features'
MENU = (FEATURES / 'featuremenu.cpp').read_text(encoding='utf-8-sig')
BRIDGE = (FEATURES / 'rendererbridge.c').read_text(encoding='utf-8-sig')
DEFAULTS = (FEATURES / 'wrapperdefaults.h').read_text(encoding='utf-8-sig')

def function(source, name):
    match = re.search(r'(?ms)^(?:static )?(?:void|bool|int|BOOL)\s+' +
                      re.escape(name) + r'\([^;{}]*\)\s*\{.*?^\}', source)
    if not match:
        raise AssertionError('Missing function: ' + name)
    return re.sub(r'/\*.*?\*/|//[^\n]*', '', match.group(), flags=re.S)

RESET = function(MENU, 'resetWrapperSettings')

def settings(name):
    body = re.search(r'static const Setting ' + name + r'\[\] = \{(.*?)\n\};',
                     DEFAULTS, re.S).group(1)
    pairs = re.findall(r'\{"([^"]+)", "((?:[^"\\]|\\.)*)"\}', body)
    if len(dict(pairs)) != len(pairs):
        raise AssertionError('Duplicate defaults: ' + name)
    return dict(pairs)

class ResetContract(unittest.TestCase):
    def test_protected_state_has_no_calls_or_writes(self):
        self.assertNotRegex(RESET, r'\b(?:timerhost|pluginhost)_[A-Za-z_]+\s*\(')
        self.assertNotIn('"C4plugins.ini"', RESET)
        self.assertNotIn('"Timer"', RESET)
        self.assertNotIn('"Settings"', RESET)
        self.assertNotRegex(RESET, r'\b(?:setNativeSpeed|setNativeCloudVisibility|gameSettings)\s*\(')
        self.assertNotRegex(RESET, r'g_attackPlaybackActive\s*=')

    def test_process_exit_is_only_through_scoped_bridge(self):
        self.assertNotRegex(RESET, r'\b(?:TerminateProcess|ExitProcess|CreateProcess\w*|'
                            r'SetProcessAffinityMask|SetThreadAffinityMask|SetPriorityClass|'
                            r'SetThreadPriority|SuspendThread|ResumeThread)\s*\(')
        self.assertEqual(RESET.count('DDExitClientAfterSettingsChange(1)'), 1)

    def test_exit_only_after_confirmed_successful_transaction(self):
        self.assertIn('MB_DEFBUTTON2', RESET)
        transaction = RESET.index('c4_ini_reset::Apply(entries)')
        failed = RESET.index('if (!saved.success)', transaction)
        failure_return = RESET.index('return;', failed)
        self.assertGreater(RESET.index('DDExitClientAfterSettingsChange(1)'), failure_return)

    def test_no_ini_delete_or_section_replacement(self):
        self.assertNotRegex(RESET, r'\b(?:DeleteFile\w*|MoveFile\w*|ReplaceFile\w*|'
                            r'WritePrivateProfileSection\w*)\s*\(')
        self.assertIn('DDGetConfigWriteTarget(', RESET)
        self.assertIn('rendererPath, "ddraw", setting.key, setting.value', RESET)
        self.assertIn('rendererPath, rendererSection, setting.key, setting.value', RESET)

    def test_render_defaults_match_distributed_ini(self):
        release = (ROOT / 'c4ddraw/release/ddraw.ini').read_text(encoding='utf-8-sig')
        distributed = dict(re.findall(r'^([^;\[\]\r\n=]+)=([^\r\n]*)$', release, re.M))
        for key, value in settings('renderer').items():
            # C string escaping, not an INI transformation.
            value = value.replace('\\\\', '\\')
            if key in distributed:
                self.assertEqual(value, distributed[key], key)
        self.assertEqual(settings('renderer')['d3d9_filter'], '3')
        self.assertEqual(settings('renderer')['maxgameticks'], '180')

    def test_timer_and_native_keys_absent_from_default_tables(self):
        keys = set().union(*(settings(name) for name in ('renderer', 'menu', 'wrapper')))
        self.assertFalse(keys & {'AutoBattle', 'Timetable', 'PauseOn', 'DayTurn',
                                'PauseAnimation', 'BattleSpeed', 'PlayerSpeed', 'OpponentSpeed'})
        self.assertEqual(settings('wrapper'), {'Archive': '1', 'IncludeSubdirectories': '0'})

    def test_no_live_mutations_or_stale_persist_after_reset(self):
        self.assertNotRegex(RESET, r'\bg_[A-Za-z_]+\s*=(?!=)')
        self.assertNotRegex(RESET, r'\b(?:persist|widebattle_set_enabled|fastai_set_enabled|'
                            r'DDSetMaxGameTicksLive|DDResetWindowPlacement|DDSetWindowStretchPercent|'
                            r'horplus_set_window_stretch_percent|localization_set_locale|'
                            r'applyAlwaysActive|applyAnimFactor|applyPerUnitBurst|applyDdrawLive|'
                            r'stageChromeForTargetMode|syncChrome|refreshChecks)\s*\(')
        persist = function(MENU, 'persist')
        for key in ('messageBatching', 'debugLog', 'netTrace'):
            self.assertNotIn('"' + key + '"', persist)

    def test_startup_policies_remain_latched(self):
        self.assertNotRegex(RESET, r'\b(?:messagebatch_install|eventtrace_install|c4trace_init)\s*\(')
        exit_helper = function(BRIDGE, 'DDExitClientAfterSettingsChange')
        self.assertNotRegex(exit_helper, r'\b(?:cfg_load|DDSetMaxGameTicksLive|'
                            r'SetProcessAffinityMask|SetThreadAffinityMask|SetPriorityClass|'
                            r'SetThreadPriority)\s*\(')

    def test_exit_disables_only_stale_geometry_save_when_requested(self):
        exit_helper = function(BRIDGE, 'DDExitClientAfterSettingsChange')
        self.assertRegex(exit_helper, r'if\s*\((?:discardOldWindowState|discard_old_window_state)\)\s*\{?\s*'
                                     r'g_config\.save_settings\s*=\s*0;')
        self.assertLess(exit_helper.index('g_config.save_settings = 0;'),
                        exit_helper.index('ExitProcess(0);'))
        self.assertNotRegex(exit_helper, r'\b(?:WritePrivateProfile\w*|'
                            r'pluginhost_[A-Za-z_]+|timerhost_[A-Za-z_]+|'
                            r'CreateProcess\w*|TerminateProcess)\s*\(')

    def test_exit_keeps_existing_upstream_termination_policy(self):
        exit_helper = function(BRIDGE, 'DDExitClientAfterSettingsChange')
        self.assertRegex(exit_helper, r'if\s*\(g_config\.terminate_process\)\s*\{?\s*'
                                     r'g_config\.terminate_process\s*=\s*2;')

    def test_fake_exit_fixture_matches_production_helper(self):
        fixture = (ROOT / 'c4ddraw/tests/settings_exit_tests.cpp').read_text(encoding='utf-8-sig')
        actual = function(BRIDGE, 'DDExitClientAfterSettingsChange')
        fake = function(fixture, 'DDExitClientAfterSettingsChange')
        self.assertEqual(re.sub(r'\s+', '', actual), re.sub(r'\s+', '', fake))
        self.assertIn('#define ExitProcess recordExit', fixture)

    def test_cfg_save_gate_prevents_detach_geometry_overwrite(self):
        config = (ROOT / 'c4ddraw/build/cnc-ddraw/src/config.c').read_text(encoding='utf-8-sig')
        save = function(config, 'cfg_save')
        self.assertRegex(save, r'if\s*\(!g_config\.save_settings\)\s*return;')
        self.assertLess(save.index('if (!g_config.save_settings)'),
                        save.index('WritePrivateProfileString('))

    def test_diagnostics_commit_only_its_key_before_exit(self):
        toggle = function(MENU, 'toggleNetworkTrace')
        self.assertIn('MB_DEFBUTTON2', toggle)
        self.assertRegex(toggle, r'"menu"\s*,\s*"netTrace"')
        self.assertEqual(toggle.count('DDExitClientAfterSettingsChange(0)'), 1)
        self.assertNotIn('DDExitClientAfterSettingsChange(1)', toggle)
        transaction = toggle.index('c4_ini_reset::Apply(entries)')
        failed = toggle.index('if (!saved.success)', transaction)
        failure_return = toggle.index('return;', failed)
        self.assertGreater(toggle.index('DDExitClientAfterSettingsChange(0)'), failure_return)
        self.assertNotIn('c4defaults::', toggle)
        self.assertNotIn('"ddraw"', toggle)
        self.assertNotIn('"C4plugins.ini"', toggle)
        self.assertNotRegex(toggle, r'\b(?:WritePrivateProfile\w*|persist|'
                            r'pluginhost_[A-Za-z_]+|timerhost_[A-Za-z_]+|'
                            r'messagebatch_install|eventtrace_install|c4trace_init|'
                            r'applyDdrawLive|SetEnvironmentVariable\w*)\s*\(')

    def test_no_automatic_restart_process_or_game_save(self):
        for code in (RESET, function(MENU, 'toggleNetworkTrace')):
            self.assertNotRegex(code, r'\b(?:CreateProcess\w*|ShellExecute\w*|'
                                r'WinExec|SendMessage\w*|PostMessage\w*)\s*\(')
            self.assertIn('NOT saved automatically', code)

    def test_forced_environment_cannot_be_silently_disabled(self):
        toggle = function(MENU, 'toggleNetworkTrace')
        guard = toggle.index('if (c4trace_environment_forced())')
        first_return = toggle.index('return;', guard)
        self.assertLess(first_return, toggle.index('c4_ini_reset::Apply(entries)'))

    def test_build_copies_required_headers(self):
        build = (ROOT / 'c4ddraw/build.ps1').read_text(encoding='utf-8-sig')
        for name in ('wrapperdefaults.h', 'inisettingsreset.h'):
            self.assertIn('features\\' + name, build)
            self.assertIn('src\\' + name, build)

if __name__ == '__main__':
    unittest.main(verbosity=2)
