# Release with assert (FX14-CI-002, item A): the checks run on the same
# optimized build that goes into the packages, with assert() kept — NDEBUG is removed from the Release flags. Whether
# a build is optimized (time limits written for an optimized build, perf thresholds) is told by
# CONSTRUE_OPTIMIZED_BUILD, not by NDEBUG. Included by the top-level CMakeLists.txt before the modules.
option(CONSTRUE_RELEASE_ASSERTS "Release keeps assert(): NDEBUG is not defined" ON)
if(CONSTRUE_RELEASE_ASSERTS)
  foreach(_flags CMAKE_CXX_FLAGS_RELEASE CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_RELWITHDEBINFO)
    # A directory variable shadows the cache entry for this folder and its subfolders.
    string(REGEX REPLACE "(^| )[/-]DNDEBUG( |$)" " " ${_flags} "${${_flags}}")
  endforeach()
endif()
add_compile_definitions($<$<NOT:$<CONFIG:Debug>>:CONSTRUE_OPTIMIZED_BUILD>)
