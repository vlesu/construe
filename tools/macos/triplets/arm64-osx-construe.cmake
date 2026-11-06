# vcpkg triplet of the macOS package (S8-PKG-003, the macOS package script): as the built-in arm64-osx (static libraries,
# so OCCT is linked into the program — LGPL relink kit), plus the oldest macOS the package supports
# (13.3: floating-point std::to_chars of the SDK's libc++).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3)
# OCCT without fused multiply-add (S8-PKG-003): Clang on arm64 contracts a*b+c into FMA by default (-ffp-contract=on),
# x86-64 builds do not; with FMA the shell of the FX7-DRW-001 part failed (MakeThickSolidByJoin kept the volume, the
# fallback gave an invalid solid) — checked on the Mac 2026-11-04: with -ffp-contract=off it passes.
if(PORT STREQUAL "opencascade")
    set(VCPKG_C_FLAGS "-ffp-contract=off")
    set(VCPKG_CXX_FLAGS "-ffp-contract=off")
endif()
