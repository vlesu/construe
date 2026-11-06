// OpenGL, scaling and diagnostics setup before QApplication is created (docs/dev/client.md §2,
// spec/app-ux.md §12.1). Implementation — app/gl_setup.cpp.
#pragma once

namespace construe::app {

// Default OpenGL format; in test mode — scale 1.0, software OpenGL, crash reports
// to stderr instead of modal CRT dialogs.
void configureBeforeApplication(bool testMode);

// Automatic fallback to software OpenGL (docs/dev/client.md §2а, FX3-PERF-004). A graphics card with a GL_VERSION
// older than OpenGL 3.0 (the view requires GLSL 1.30) needs software OpenGL; an unrecognized string — no (Qt decides).
bool glVersionNeedsSoftware(const char* glVersion);
// Whether to fall back automatically: normal (non-test) mode, QT_OPENGL not set, probe gave a version older than 3.0.
bool shouldAutoSwitchToSoftwareGl(bool testMode, bool qtOpenGlSet, const char* probedVersion);
// Graphics card GL_VERSION that made the program fall back to software OpenGL; empty — no fallback happened.
const char* glAutoSoftwareVersion();
// Probe: desktop OpenGL GL_VERSION of the graphics card via a temporary invisible context (Windows, WGL); empty — no
// context created or not Windows. The string lives until the next call.
const char* probeDesktopGlVersion();

}  // namespace construe::app
