# Optional build speed-ups (FX14-CI-001), used by the linux-cloud preset: the compiler cache ccache and a fast linker.
# Both are optional: a missing program gives a message and the usual build, never an error.
#   CONSTRUE_CCACHE=ON       compile through ccache with the settings file cmake/ccache.conf when it exists
#                            (CCACHE_CONFIGPATH) and paths hashed relative to the source root (CCACHE_BASEDIR).
#                            GCC/Clang: the launcher «env ... ccache». MSVC (off by default, not yet checked on Windows;
#                            how to check — the developer setup notes, «ccache для MSVC»): Ninja generator only; CONSTRUE_CCACHE_LAUNCHER
#                            «cmake -E env ... ccache» goes after tools/cl_launcher.py (CMakeLists.txt), the cache folder
#                            is CONSTRUE_CCACHE_DIR (default %LOCALAPPDATA%/construe-dev/ccache), and the debug
#                            information of Debug/RelWithDebInfo goes into the objects (/Z7 instead of a shared PDB, which
#                            ccache cannot cache); the linker still writes the .pdb of every program.
#   CONSTRUE_LINKER=<name>   link with -fuse-ld=<name> (mold, lld, gold) when the program ld.<name> or <name> is found.
option(CONSTRUE_CCACHE "Compile through ccache (cmake/ccache.conf)" OFF)
set(CONSTRUE_LINKER "" CACHE STRING "Linker for -fuse-ld (mold, lld, gold); empty — the default one")

set(CONSTRUE_CCACHE_LAUNCHER "")
if(CONSTRUE_CCACHE)
  find_program(CONSTRUE_CCACHE_PROGRAM ccache)
  if(NOT CONSTRUE_CCACHE_PROGRAM OR NOT EXISTS "${CONSTRUE_CCACHE_PROGRAM}")
    message(STATUS "CONSTRUE_CCACHE: ccache not found, building without the compiler cache")
  elseif(MSVC)
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
      message(STATUS "CONSTRUE_CCACHE: with MSVC only the Ninja generator is supported, building without the cache")
    else()
      if(NOT CONSTRUE_CCACHE_DIR)
        file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}/construe-dev/ccache" _ccache_dir)
        set(CONSTRUE_CCACHE_DIR "${_ccache_dir}" CACHE PATH "ccache folder of the MSVC build")
      endif()
      set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
      set(CONSTRUE_CCACHE_LAUNCHER "${CMAKE_COMMAND}" -E env "CCACHE_CONFIGPATH=${PROJECT_SOURCE_DIR}/cmake/ccache.conf"
          "CCACHE_BASEDIR=${PROJECT_SOURCE_DIR}" "CCACHE_DIR=${CONSTRUE_CCACHE_DIR}" "${CONSTRUE_CCACHE_PROGRAM}")
      message(STATUS "CONSTRUE_CCACHE: ${CONSTRUE_CCACHE_PROGRAM} (MSVC, ${CONSTRUE_CCACHE_DIR})")
    endif()
  elseif(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    set(CMAKE_CXX_COMPILER_LAUNCHER env "CCACHE_CONFIGPATH=${PROJECT_SOURCE_DIR}/cmake/ccache.conf"
        "CCACHE_BASEDIR=${PROJECT_SOURCE_DIR}" "${CONSTRUE_CCACHE_PROGRAM}")
    message(STATUS "CONSTRUE_CCACHE: ${CONSTRUE_CCACHE_PROGRAM}")
  endif()
endif()

if(CONSTRUE_LINKER AND NOT MSVC)
  find_program(CONSTRUE_LINKER_PROGRAM NAMES ld.${CONSTRUE_LINKER} ${CONSTRUE_LINKER})
  if(CONSTRUE_LINKER_PROGRAM)
    add_link_options(-fuse-ld=${CONSTRUE_LINKER})
    message(STATUS "CONSTRUE_LINKER: ${CONSTRUE_LINKER_PROGRAM}")
  else()
    message(STATUS "CONSTRUE_LINKER: ${CONSTRUE_LINKER} not found, the default linker is used")
  endif()
endif()
