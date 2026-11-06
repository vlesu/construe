// Console import and history reconstruction (FX10-REC-001, reconstruction.md §12). Exit code: 0 — done, 1 — the file was
// not processed, 2 — a wrong call.
#include "rec_cli/rec_cli.h"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>

#include <shellapi.h>
#endif

namespace {

// Arguments in UTF-8 (file paths in the session are UTF-8): on Windows argv is in the local code page.
std::vector<std::string> arguments(int argc, char** argv) {
#ifdef _WIN32
    (void)argv;
    std::vector<std::string> out;
    int n = 0;
    LPWSTR* w = CommandLineToArgvW(GetCommandLineW(), &n);
    for (int i = 1; i < n; ++i) {
        const int len = WideCharToMultiByte(CP_UTF8, 0, w[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(len), 0);
        WideCharToMultiByte(CP_UTF8, 0, w[i], -1, s.data(), len, nullptr, nullptr);
        s.pop_back();  // the terminating zero
        out.push_back(s);
    }
    LocalFree(w);
    return out;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    using namespace construe::rec_cli;
    Options o;
    std::string error;
    if (!parseArgs(arguments(argc, argv), o, error)) {
        std::cerr << error << "\n" << usage();
        return 2;
    }
    const auto log = run(o);
    std::cout << summary(log);
    return log.value("status", "") == "ok" ? 0 : 1;
}
