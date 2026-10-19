#pragma once
// ============================================================================
// src/diag/input_trace.h -- the hands-on test logger (OPENBA_INPUT_TRACE=1)
// ============================================================================
// Writes input_trace.log (one line per click / wheel / key / window event, mouse moves rate
// limited, plus a once-a-second S summary) in the process's working directory, the folder
// that also holds standalone_debug.log. Nothing in the game calls it: when the Opt() flag
// is on, a tiny helper thread (started by a static in input_trace.cpp) waits for SDL's video
// subsystem and then adds an SDL event watch. With the flag off the helper exits at once and
// the cost is zero. Format and use: docs/macos/HANDTEST.md, tools/macos/handtest/analyze.py.
// ============================================================================

namespace Diag {
namespace InputTrace {

// Install the watch now and log to `path` (used by the helper thread and by
// tools/macos/handtest/trace_harness.cpp). Returns false when already installed or the
// file cannot be opened. SDL's video subsystem must be initialised.
bool Install(const char* path);

// Write the final line, remove the watch and close the file (also run at process exit).
void Shutdown();

}  // namespace InputTrace
}  // namespace Diag
