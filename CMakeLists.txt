cmake_minimum_required(VERSION 3.25)

project(construe VERSION 0.1.0 LANGUAGES CXX)
# Pre-release label of the version (docs/dev/spec/update.md §2; SemVer: 0.1.0-alpha precedes 0.1.0); empty — a release.
# CMake's project(VERSION) takes only numbers, so the label lives here; package scripts read this line.
set(CONSTRUE_VERSION_PRERELEASE "alpha")
if(CONSTRUE_VERSION_PRERELEASE STREQUAL "")
  set(CONSTRUE_VERSION_FULL "${PROJECT_VERSION}")
else()
  set(CONSTRUE_VERSION_FULL "${PROJECT_VERSION}-${CONSTRUE_VERSION_PRERELEASE}")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CONSTRUE_BUILD_APP "Build the Qt application and e2e tests" ON)
option(CONSTRUE_BUILD_TESTS "Build unit and conformance tests" ON)
# The public source snapshot has no tests/ folder (S8-REPO-002): the program is built without tests, the options above
# stay harmless.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
  set(CONSTRUE_HAVE_TESTS ON)
else()
  set(CONSTRUE_HAVE_TESTS OFF)
endif()

if(MSVC)
  # /utf-8: sources and strings are UTF-8; /permissive-: strict standard conformance.
  add_compile_options(/utf-8 /permissive- /W4 /EHsc /Zc:__cplusplus)
  add_compile_definitions(_USE_MATH_DEFINES NOMINMAX)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

find_package(Eigen3 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

include(cmake/ConstrueModule.cmake)
include(cmake/ReleaseAsserts.cmake)  # Release keeps assert() (FX14-CI-002)
# ccache and a fast linker of the cloud preset (FX14-CI-001); both optional.
include(cmake/BuildCache.cmake)
# Ninja + localized MSVC: the /showIncludes lines are converted to the OEM encoding of the prefix
# (tools/cl_launcher.py, docs/dev/setup.md «Header dependencies»).
if(MSVC AND CMAKE_GENERATOR MATCHES "Ninja")
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  # With CONSTRUE_CCACHE=ON the launcher runs ccache, which runs cl (cmake/BuildCache.cmake).
  set(CMAKE_CXX_COMPILER_LAUNCHER "${Python3_EXECUTABLE}" -I -E "${PROJECT_SOURCE_DIR}/tools/cl_launcher.py"
      ${CONSTRUE_CCACHE_LAUNCHER})
endif()

if(CONSTRUE_BUILD_APP)
  set(construe_qt_test)
  if(CONSTRUE_HAVE_TESTS)
    set(construe_qt_test Test)  # e2e and unit tests of the application
  endif()
  find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets Network PrintSupport ${construe_qt_test} GuiPrivate)
endif()

add_subdirectory(src)

if(CONSTRUE_BUILD_TESTS AND CONSTRUE_HAVE_TESTS)
  enable_testing()
  find_package(GTest CONFIG REQUIRED)
  include(GoogleTest)
  add_subdirectory(tests)
endif()
