# FastAI dispatch contract tests

The harness includes the actual `features/fastai.cpp`, but never calls its EXE hook installer or
window-discovery scanner. It creates and destroys only its own hidden windows. No game, MSS, INI,
network endpoint, CPU affinity, priority or timing configuration is touched.

Run from the repository root with Visual Studio x86 C++ tools installed:

```powershell
./c4ddraw/tests/run-fastai-dispatch-tests.ps1 -Configuration Debug
./c4ddraw/tests/run-fastai-dispatch-tests.ps1 -Configuration Release
```

Each run uses a new directory under `.diagnostics` and preserves build/test logs and source hashes.
The runner also compiles the production translation unit without the test API substitutions.

Covered behavior:

- Real queued timer keeps its actual ID; registered TIMERPROC executes through DispatchMessage.
- Child HWND uses its own WndProc, unrelated posted HWND messages remain queued.
- WM_QUIT is preserved once with its exit code; close/destruction runs subclass cleanup.
- Empty-queue synthetic ticks retain the original direct-native timer ID/parameter contract.
- Budget expiration or backward QPC after Sleep stops before Peek/removal; after empty Peek it
  prevents a synthetic tick. A real message already removed by a slow Peek is delivered once.
- Nested slices are rejected, native sent callbacks still run, trace source/depth stays accurate.
- Teardown inside Peek or a synthetic callback prevents further calls to a stale attachment.
- SEH propagation and QPC failure clear pump/provenance/trace guards; stale generation cannot detach
  the current window; outer native result and LastError survive optional acceleration.
- Trace enabled/disabled execution is checked. Source 4 is reserved for old recordings and is no
  longer emitted: a real timer is never consumed as a replacement synthetic timer. Source 2 denotes
  a real queued message's native server forward. Child HWND/TIMERPROC dispatch may not reach that
  server forward and therefore does not necessarily produce events 70/71.

QPC and Sleep are deterministic test seams. The dispatch-limit test forces an empty Peek result to
avoid unrelated OS broadcasts; the window/queued-timer tests use real Windows message dispatch.
Two injected SEH failures exercise the actual finally scopes without relying on Windows callback
exception-swallowing policy. The teardown-inside-Peek test models a sent callback using synchronous
DestroyWindow on the harness's own HWND.

The 2026-09-04 deadline-review runs passed 193 checks with zero failures in both configurations:
`.diagnostics/fastai-dispatch-tests-20260904-162141-993` (Debug) and
`.diagnostics/fastai-dispatch-tests-20260904-162153-061` (Release). These add deterministic elapsed-time
and backward-clock injection at both yielding boundaries, plus overbudget real-message delivery.

These tests verify dispatch semantics, not game performance, animation timing, or a fix for the MSS
post-then-store notification race. The 3 ms budget is checked between callbacks and cannot preempt a
single long native callback. Full wrapper build and separately authorized game validation remain
distinct verification steps.
