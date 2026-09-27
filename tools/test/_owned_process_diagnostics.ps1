#requires -Version 7.0

# Read-only diagnostics for a Process object created by the current harness.
# Nothing here discovers a process by name or PID, changes window state, sends
# input, or terminates a process. The PID is read only from the retained owned
# Process handle and is used to filter the Win32 window census.
if (-not ([System.Management.Automation.PSTypeName]'D2Mss.TestOwnedWindowProbe').Type) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

namespace D2Mss
{
    public sealed class TestOwnedWindowInfo
    {
        public string Handle { get; set; }
        public string Parent { get; set; }
        public string Owner { get; set; }
        public uint ThreadId { get; set; }
        public string ClassName { get; set; }
        public string Text { get; set; }
        public bool TopLevel { get; set; }
        public bool Visible { get; set; }
        public bool Enabled { get; set; }
        public int Left { get; set; }
        public int Top { get; set; }
        public int Right { get; set; }
        public int Bottom { get; set; }
    }

    public static class TestOwnedWindowProbe
    {
        private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

        [StructLayout(LayoutKind.Sequential)]
        private struct Rect
        {
            public int Left;
            public int Top;
            public int Right;
            public int Bottom;
        }

        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern bool EnumChildWindows(IntPtr parent, EnumWindowsProc callback,
                                                    IntPtr lParam);

        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassName(IntPtr hWnd, StringBuilder value, int capacity);

        [DllImport("user32.dll")]
        private static extern bool IsWindowVisible(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern bool IsWindowEnabled(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern IntPtr GetParent(IntPtr hWnd);

        [DllImport("user32.dll")]
        private static extern IntPtr GetWindow(IntPtr hWnd, uint command);

        [DllImport("user32.dll")]
        private static extern bool GetWindowRect(IntPtr hWnd, out Rect rect);

        [DllImport("user32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint message,
                                                        IntPtr wParam, StringBuilder lParam,
                                                        uint flags, uint timeout,
                                                        out IntPtr result);

        private const uint GetOwner = 4;
        private const uint GetText = 0x000D;
        private const uint AbortIfHung = 0x0002;

        private static string Hex(IntPtr value)
        {
            return "0x" + unchecked((ulong)value.ToInt64()).ToString("x");
        }

        private static string ReadClass(IntPtr hWnd)
        {
            var value = new StringBuilder(512);
            return GetClassName(hWnd, value, value.Capacity) > 0 ? value.ToString() : "";
        }

        private static string ReadText(IntPtr hWnd)
        {
            var value = new StringBuilder(4096);
            IntPtr ignored;
            return SendMessageTimeout(hWnd, GetText, (IntPtr)value.Capacity, value,
                                      AbortIfHung, 100, out ignored) != IntPtr.Zero
                       ? value.ToString()
                       : "";
        }

        private static void AddWindow(List<TestOwnedWindowInfo> result, HashSet<long> seen,
                                      IntPtr hWnd, bool topLevel)
        {
            if (!seen.Add(hWnd.ToInt64()))
                return;
            uint processId;
            uint threadId = GetWindowThreadProcessId(hWnd, out processId);
            Rect rect;
            bool hasRect = GetWindowRect(hWnd, out rect);
            result.Add(new TestOwnedWindowInfo {
                Handle = Hex(hWnd),
                Parent = Hex(GetParent(hWnd)),
                Owner = Hex(GetWindow(hWnd, GetOwner)),
                ThreadId = threadId,
                ClassName = ReadClass(hWnd),
                Text = ReadText(hWnd),
                TopLevel = topLevel,
                Visible = IsWindowVisible(hWnd),
                Enabled = IsWindowEnabled(hWnd),
                Left = hasRect ? rect.Left : 0,
                Top = hasRect ? rect.Top : 0,
                Right = hasRect ? rect.Right : 0,
                Bottom = hasRect ? rect.Bottom : 0
            });
        }

        public static TestOwnedWindowInfo[] Capture(int processId)
        {
            var result = new List<TestOwnedWindowInfo>();
            var seen = new HashSet<long>();
            EnumWindows(delegate(IntPtr top, IntPtr ignored) {
                uint ownerProcessId;
                GetWindowThreadProcessId(top, out ownerProcessId);
                if (ownerProcessId != unchecked((uint)processId))
                    return true;
                AddWindow(result, seen, top, true);
                EnumChildWindows(top, delegate(IntPtr child, IntPtr childIgnored) {
                    uint childProcessId;
                    GetWindowThreadProcessId(child, out childProcessId);
                    if (childProcessId == unchecked((uint)processId))
                        AddWindow(result, seen, child, false);
                    return true;
                }, IntPtr.Zero);
                return true;
            }, IntPtr.Zero);
            return result.ToArray();
        }
    }
}
'@
}

function Get-OwnedProcessDiagnostic {
    param(
        [Parameter(Mandatory)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory)][string]$Role,
        [Parameter(Mandatory)][string]$Reason
    )

    $ownedPid = $Process.Id
    $Process.Refresh()
    $hasExited = $Process.HasExited
    $threads = @()
    $windows = @()
    $snapshotError = $null

    if (-not $hasExited) {
        try {
            $windows = @([D2Mss.TestOwnedWindowProbe]::Capture($ownedPid))
        } catch {
            $snapshotError = "window census failed: $($_.Exception.Message)"
        }
        try {
            $threadRows = [System.Collections.Generic.List[object]]::new()
            foreach ($thread in $Process.Threads) {
                $state = $null
                $waitReason = $null
                $cpuMilliseconds = $null
                $startAddress = $null
                try { $state = [string]$thread.ThreadState } catch {}
                if ($state -eq 'Wait') {
                    try { $waitReason = [string]$thread.WaitReason } catch {}
                }
                try { $cpuMilliseconds = [math]::Round($thread.TotalProcessorTime.TotalMilliseconds, 3) } catch {}
                try { $startAddress = ('0x{0:x}' -f [long]$thread.StartAddress) } catch {}
                $threadRows.Add([pscustomobject]@{
                    id = $thread.Id
                    state = $state
                    waitReason = $waitReason
                    cpuMilliseconds = $cpuMilliseconds
                    startAddress = $startAddress
                })
            }
            $threads = @($threadRows)
        } catch {
            $message = "thread census failed: $($_.Exception.Message)"
            $snapshotError = if ($snapshotError) { "$snapshotError | $message" } else { $message }
        }
    }

    $responding = $null
    $mainWindowHandle = $null
    $mainWindowTitle = $null
    $cpuMilliseconds = $null
    $workingSetBytes = $null
    $privateMemoryBytes = $null
    $handleCount = $null
    $exitCode = $null
    if ($hasExited) {
        try { $exitCode = $Process.ExitCode } catch {}
    } else {
        try { $responding = $Process.Responding } catch {}
        try { $mainWindowHandle = ('0x{0:x}' -f [long]$Process.MainWindowHandle) } catch {}
        try { $mainWindowTitle = $Process.MainWindowTitle } catch {}
        try { $cpuMilliseconds = [math]::Round($Process.TotalProcessorTime.TotalMilliseconds, 3) } catch {}
        try { $workingSetBytes = $Process.WorkingSet64 } catch {}
        try { $privateMemoryBytes = $Process.PrivateMemorySize64 } catch {}
        try { $handleCount = $Process.HandleCount } catch {}
    }

    return [pscustomobject]@{
        timestamp = (Get-Date).ToUniversalTime().ToString('o')
        role = $Role
        reason = $Reason
        pid = $ownedPid
        hasExited = $hasExited
        exitCode = $exitCode
        responding = $responding
        mainWindowHandle = $mainWindowHandle
        mainWindowTitle = $mainWindowTitle
        cpuMilliseconds = $cpuMilliseconds
        workingSetBytes = $workingSetBytes
        privateMemoryBytes = $privateMemoryBytes
        handleCount = $handleCount
        windows = @($windows)
        threads = @($threads)
        snapshotError = $snapshotError
    }
}

