# Common build functions of the modules (docs/dev/architecture.md §2).
# Each core module is a static library construe_<name> with an explicit
# list of dependencies: the dependency rules are checked by the build.

function(construe_add_module name)
  cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;PRIVATE_DEPS" ${ARGN})
  add_library(construe_${name} STATIC ${ARG_SOURCES})
  add_library(construe::${name} ALIAS construe_${name})
  target_include_directories(construe_${name} PUBLIC ${PROJECT_SOURCE_DIR}/src)
  target_link_libraries(construe_${name} PUBLIC ${ARG_DEPS} PRIVATE ${ARG_PRIVATE_DEPS})
endfunction()
