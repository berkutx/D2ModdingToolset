# Per-machine test config. Copy this file to test.config.psd1 (gitignored) and set the paths
# for your machine. The relay-driven tests read it when you don't pass -GameDir explicitly.
@{
    # Your Disciples 2 install, the folder with Discipl2.exe and the renamed Mss23.dll, with the
    # DebugTest mss32.dll deployed over it.
    GameDir  = 'C:\GOG Games\slasher_mns_2_4'

    # Optional: full path to procdump.exe for crash capture in the multiplayer test ('' = skip).
    ProcDump = ''
}
