// Process setup before QApplication is created (spec/app-ux.md §12.1) and crash diagnostics.
// This file deliberately includes no project headers: windows.h/dbghelp.h break Ninja's dependency
// recording for the translation unit (docs/dev/setup.md), so there are no dependencies here
// that could go stale. The declaration is in app/gl_setup.h.
#include <QByteArray>
#include <QSurfaceFormat>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#ifdef _MSC_VER
#include <crtdbg.h>
#include <stdlib.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <GL/gl.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "opengl32.lib")
#endif

namespace construe::app {

#ifdef _MSC_VER
inline void printStackTrace();
#endif

// Unhandled exception — text to stderr before abnormal termination (diagnostics).
inline void installTerminateReporter() {
    std::set_terminate([] {
        if (auto e = std::current_exception()) {
            try {
                std::rethrow_exception(e);
            } catch (const std::exception& ex) {
                std::fprintf(stderr, "construe: unhandled exception: %s\n", ex.what());
            } catch (...) {
                std::fprintf(stderr, "construe: unhandled non-standard exception\n");
            }
        }
#ifdef _MSC_VER
        printStackTrace();
#endif
        std::fflush(stderr);
        std::abort();
    });
}

#ifdef _MSC_VER
// Call stack of the current thread to stderr (test mode: crash report).
inline void printStackTrace() {
    HANDLE proc = GetCurrentProcess();
    static bool init = false;
    if (!init) {
        SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME);
        SymInitialize(proc, nullptr, TRUE);
        init = true;
    }
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    alignas(SYMBOL_INFO) char buf[sizeof(SYMBOL_INFO) + 256];
    auto* sym = reinterpret_cast<SYMBOL_INFO*>(buf);
    for (USHORT i = 0; i < n; ++i) {
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        const char* name = SymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?";
        if (SymGetLineFromAddr64(proc, addr, &ldisp, &line)) std::fprintf(stderr, "  #%u %s  %s:%lu\n", i, name, line.FileName, line.LineNumber);
        else std::fprintf(stderr, "  #%u %s\n", i, name);
    }
    std::fflush(stderr);
}
#endif

// Declarations are in app/gl_setup.h (not included here, see the top of the file).
bool glVersionNeedsSoftware(const char* glVersion) {
    if (!glVersion) return false;
    const char* p = glVersion;
    if (std::strncmp(p, "OpenGL ES ", 10) == 0) p += 10;
    int major = 0, digits = 0;
    while (*p >= '0' && *p <= '9' && digits < 4) major = major * 10 + (*p++ - '0'), ++digits;
    if (digits == 0 || *p != '.') return false;  // not «major.minor…» — not recognized
    return major < 3;
}

bool shouldAutoSwitchToSoftwareGl(bool testMode, bool qtOpenGlSet, const char* probedVersion) {
    return !testMode && !qtOpenGlSet && glVersionNeedsSoftware(probedVersion);
}

namespace {

std::string g_autoSoftwareVersion;  // graphics card GL_VERSION on automatic fallback
std::string g_probed;               // result of the last probe

}  // namespace

#ifdef _MSC_VER
// Desktop OpenGL GL_VERSION of the graphics card: a temporary invisible window and a WGL context (the compatibility
// profile gives the highest supported version); empty — no context created (Qt decides). Before QApplication, no Qt.
const char* probeDesktopGlVersion() {
    std::string& out = g_probed;
    out.clear();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = inst;
    wc.lpszClassName = L"construe_gl_probe";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return out.c_str();
    HDC dc = GetDC(hwnd);
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = dc ? ChoosePixelFormat(dc, &pfd) : 0;
    if (pf && SetPixelFormat(dc, pf, &pfd)) {
        if (HGLRC rc = wglCreateContext(dc)) {
            if (wglMakeCurrent(dc, rc)) {
                if (const auto* v = reinterpret_cast<const char*>(glGetString(GL_VERSION))) out = v;
                wglMakeCurrent(nullptr, nullptr);
            }
            wglDeleteContext(rc);
        }
    }
    if (dc) ReleaseDC(hwnd, dc);
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, inst);
    return out.c_str();
}
#else
const char* probeDesktopGlVersion() { return ""; }
#endif

const char* glAutoSoftwareVersion() { return g_autoSoftwareVersion.c_str(); }

void configureBeforeApplication(bool testMode) {
    installTerminateReporter();
    QSurfaceFormat f;
    // CONSTRUE_GL_COMPAT=1 — diagnostics: an OpenGL 3.0 compatibility context with GLSL 1.30 shaders, as
    // some drivers and software OpenGL give instead of 3.3 core.
    if (qEnvironmentVariableIsEmpty("CONSTRUE_GL_COMPAT")) {
        f.setVersion(3, 3);
        f.setProfile(QSurfaceFormat::CoreProfile);
    } else {
        f.setVersion(3, 0);
        f.setProfile(QSurfaceFormat::CompatibilityProfile);
        f.setOption(QSurfaceFormat::DeprecatedFunctions);
    }
    f.setDepthBufferSize(24);
    f.setSamples(testMode ? 0 : 4);
    QSurfaceFormat::setDefaultFormat(f);
    if (testMode) {
#ifdef _MSC_VER
        // Test mode: no modal debug CRT dialogs (abort, assert) — messages go to stderr,
        // otherwise an automated run hangs until someone clicks (S1-UX-001).
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        for (int t : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(t, _CRTDBG_MODE_FILE);
            _CrtSetReportFile(t, _CRTDBG_FILE_STDERR);
        }
        // CONSTRUE_HEAPCHECK=1 — heap check on every operation (hunting memory corruption).
        if (!qEnvironmentVariableIsEmpty("CONSTRUE_HEAPCHECK"))
            _CrtSetDbgFlag(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) | _CRTDBG_CHECK_ALWAYS_DF);
        _CrtSetReportHook2(_CRT_RPTHOOK_INSTALL, [](int type, char* msg, int*) -> int {
            if (type != _CRT_WARN) {
                std::fprintf(stderr, "construe: %s", msg ? msg : "");
                printStackTrace();
            }
            return FALSE;
        });
#endif
        // Test mode: scale 1.0, software OpenGL.
        qputenv("QT_SCALE_FACTOR", "1");
        qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
        if (qgetenv("QT_OPENGL").isEmpty()) qputenv("QT_OPENGL", "software");
    }
#ifdef _MSC_VER
    // Graphics card older than OpenGL 3.0 — software OpenGL (client.md §2а, FX3-PERF-004); the message goes to the log
    // when the view is created and to «About».
    if (!testMode && qgetenv("QT_OPENGL").isEmpty()) {
        const std::string v = probeDesktopGlVersion();  // a copy: the probe string lives until the next call
        if (shouldAutoSwitchToSoftwareGl(testMode, false, v.c_str())) {
            g_autoSoftwareVersion = v;
            qputenv("QT_OPENGL", "software");
        }
    }
#endif
}

}  // namespace construe::app
