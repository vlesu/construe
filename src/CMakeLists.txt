# Core modules and the application (docs/dev/architecture.md §2).

construe_add_module(base
  SOURCES
    base/ids.cpp
    base/hash.cpp
    base/number_format.cpp
    base/log.cpp
    base/spline.cpp
    base/program_dir.cpp
    base/strings.cpp
    base/files.cpp
    base/charconv.cpp
    base/thread.cpp
  DEPS
    Eigen3::Eigen
)

construe_add_module(expr
  SOURCES
    expr/expr.cpp
    expr/variables.cpp
    expr/format.cpp
  DEPS
    construe::base
)

# Straight-toothed involute gear rims (spec/gear.md, stage 6): pure functions of geometry and profile.
construe_add_module(gear
  SOURCES
    gear/gear.cpp
  DEPS
    construe::base
)

construe_add_module(solver
  SOURCES
    solver/catalog.cpp
    solver/linalg.cpp
    solver/core.cpp
    solver/diagnose.cpp
    solver/drag.cpp
    solver/problem.cpp
    solver/system.cpp
  DEPS
    construe::base
    Eigen3::Eigen
)
target_compile_definitions(construe_solver PUBLIC EIGEN_MPL2_ONLY)

construe_add_module(font
  SOURCES
    font/font.cpp
  DEPS
    construe::base
)
# Font folders of the repository — the default library (spec/font.md §2): stencil fonts first, then the GOST fonts.
target_compile_definitions(construe_font PRIVATE CONSTRUE_FONTS_DIR_DEFAULT="${PROJECT_SOURCE_DIR}/assets/trafonts"
                                                   CONSTRUE_FONTS_DIR_DEFAULT2="${PROJECT_SOURCE_DIR}/assets/fonts")

construe_add_module(sketch
  SOURCES
    sketch/data.cpp
    sketch/sketch.cpp
    sketch/profiles.cpp
    sketch/region.cpp
    sketch/trim.cpp
    sketch/groups.cpp
    sketch/primitives.cpp
    sketch/offset.cpp
  DEPS
    construe::base
    construe::expr
    construe::solver
    construe::font
)

# ISO 286 tolerance and fit tables (S3-PMI-002) are embedded into pmi (source of truth —
# docs/dev/spec/formats/fit-tables.json).
set(CONSTRUE_FITS ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/fit-tables.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_FITS})
file(READ ${CONSTRUE_FITS} CONSTRUE_FITS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_FITS_HEX "${CONSTRUE_FITS_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/pmi/fit_tables_data.inc "${CONSTRUE_FITS_HEX}")
# Weld type table (S5-WELD-001) is embedded into pmi (source of truth — docs/dev/spec/formats/weld-types.json).
set(CONSTRUE_WELDS ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/weld-types.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_WELDS})
file(READ ${CONSTRUE_WELDS} CONSTRUE_WELDS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_WELDS_HEX "${CONSTRUE_WELDS_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/pmi/weld_types_data.inc "${CONSTRUE_WELDS_HEX}")

construe_add_module(pmi
  SOURCES
    pmi/pmi.cpp
    pmi/weld.cpp
  DEPS
    construe::base
    nlohmann_json::nlohmann_json
)
target_include_directories(construe_pmi PRIVATE ${CMAKE_BINARY_DIR}/generated)

# The document schema is embedded into the doc library as a byte array (source of truth —
# docs/dev/spec/formats/document.schema.json).
set(CONSTRUE_SCHEMA ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/document.schema.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_SCHEMA})
file(READ ${CONSTRUE_SCHEMA} CONSTRUE_SCHEMA_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_SCHEMA_HEX "${CONSTRUE_SCHEMA_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/doc/schema_data.inc "${CONSTRUE_SCHEMA_HEX}")

construe_add_module(doc
  SOURCES
    doc/json_text.cpp
    doc/schema.cpp
    doc/document.cpp
    doc/migrations.cpp
    doc/element_copy.cpp
    doc/table_text.cpp
    doc/undo.cpp
  DEPS
    construe::base
    construe::sketch
    construe::pmi
    nlohmann_json::nlohmann_json
)
target_include_directories(construe_doc PRIVATE ${CMAKE_BINARY_DIR}/generated)

# Library of standard parts (spec/library.md): the package description schema is embedded (source of truth —
# docs/dev/spec/formats/library-part.schema.json); the system library folder is assets/library.
set(CONSTRUE_LIBRARY_SCHEMA ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/library-part.schema.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_LIBRARY_SCHEMA})
file(READ ${CONSTRUE_LIBRARY_SCHEMA} CONSTRUE_LIBRARY_SCHEMA_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_LIBRARY_SCHEMA_HEX "${CONSTRUE_LIBRARY_SCHEMA_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/library/library_schema_data.inc "${CONSTRUE_LIBRARY_SCHEMA_HEX}")

construe_add_module(library
  SOURCES
    library/library.cpp
    library/catalog.cpp
    library/translations.cpp
  DEPS
    construe::base
    construe::expr
    construe::doc
)
target_include_directories(construe_library PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_compile_definitions(construe_library PRIVATE CONSTRUE_LIBRARY_DIR_DEFAULT="${PROJECT_SOURCE_DIR}/assets/library")

# Library of standard solutions (spec/solutions.md): the solution description schema is embedded (source of truth —
# docs/dev/spec/formats/solution.schema.json).
set(CONSTRUE_SOLUTION_SCHEMA ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/solution.schema.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_SOLUTION_SCHEMA})
file(READ ${CONSTRUE_SOLUTION_SCHEMA} CONSTRUE_SOLUTION_SCHEMA_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_SOLUTION_SCHEMA_HEX "${CONSTRUE_SOLUTION_SCHEMA_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/solutions/solution_schema_data.inc "${CONSTRUE_SOLUTION_SCHEMA_HEX}")

construe_add_module(solutions
  SOURCES
    solutions/solutions.cpp
    solutions/recording.cpp
  DEPS
    construe::base
    construe::expr
    construe::doc
)
target_include_directories(construe_solutions PRIVATE ${CMAKE_BINARY_DIR}/generated)

construe_add_module(kernel
  SOURCES
    kernel/profile_check.cpp
    kernel/canonical.cpp
    kernel/hlr_contour.cpp
    kernel/through_all.cpp
    kernel/axial_range.cpp
  DEPS
    construe::base
)

find_package(OpenCASCADE CONFIG REQUIRED)
construe_add_module(kernel_occt
  SOURCES
    kernel_occt/occt_kernel.cpp
    kernel_occt/occt_guard.cpp
    kernel_occt/occt_exchange.cpp
    kernel_occt/occt_hlr.cpp
    kernel_occt/occt_section.cpp
    kernel_occt/occt_build.cpp
    kernel_occt/occt_heal.cpp
  DEPS
    construe::kernel
  PRIVATE_DEPS
    TKernel TKMath TKG2d TKG3d TKGeomBase TKBRep TKGeomAlgo TKTopAlgo TKPrim TKBO TKBool TKFillet TKOffset TKMesh
    TKShHealing TKDESTEP TKXCAF TKLCAF TKCAF TKCDF TKXSBase TKDE TKService TKV3d TKHLR
)

# Sheet metal part and flat pattern (spec/sheet.md, stage 5).
construe_add_module(sheet
  SOURCES
    sheet/sheet.cpp
    sheet/build.cpp
    sheet/recognize.cpp
  DEPS
    construe::base
    construe::kernel
)

# Preparing a body for 3D printing (spec/printprep.md, stage 6): fits and physical threads for export.
construe_add_module(printprep
  SOURCES
    printprep/printprep.cpp
  DEPS
    construe::base
    construe::kernel
)

# Hole table (S2-FEAT-008) is embedded into regen (source of truth —
# docs/dev/spec/formats/hole-tables.json).
set(CONSTRUE_HOLES ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/hole-tables.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_HOLES})
file(READ ${CONSTRUE_HOLES} CONSTRUE_HOLES_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_HOLES_HEX "${CONSTRUE_HOLES_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/regen/hole_tables_data.inc "${CONSTRUE_HOLES_HEX}")
set(CONSTRUE_THREADS ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/thread-tables.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_THREADS})
file(READ ${CONSTRUE_THREADS} CONSTRUE_THREADS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_THREADS_HEX "${CONSTRUE_THREADS_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/regen/thread_tables_data.inc "${CONSTRUE_THREADS_HEX}")
set(CONSTRUE_MATERIALS ${PROJECT_SOURCE_DIR}/docs/dev/spec/formats/materials.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CONSTRUE_MATERIALS})
file(READ ${CONSTRUE_MATERIALS} CONSTRUE_MATERIALS_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," CONSTRUE_MATERIALS_HEX "${CONSTRUE_MATERIALS_HEX}")
file(WRITE ${CMAKE_BINARY_DIR}/generated/regen/materials_data.inc "${CONSTRUE_MATERIALS_HEX}")

construe_add_module(regen
  SOURCES
    regen/naming.cpp
    regen/regen.cpp
    regen/instance_vars.cpp
    regen/eval_shell.cpp
    regen/eval_sweep.cpp
    regen/eval_import.cpp
    regen/eval_assembly_features.cpp
    regen/eval_connection.cpp
    regen/eval_hole.cpp
    regen/eval_gear.cpp
    regen/eval_coil.cpp
    regen/eval_loft.cpp
    regen/eval_extrude.cpp
    regen/eval_edge.cpp
    regen/eval_sheet.cpp
    regen/mates.cpp
    regen/assembly_ref.cpp
    regen/instance_patterns.cpp
    regen/hole_tables.cpp
    regen/state_io.cpp
  DEPS
    construe::base
    construe::expr
    construe::sketch
    construe::doc
    construe::kernel
    construe::sheet
    construe::gear
    construe::solutions
)
target_include_directories(construe_regen PRIVATE ${CMAKE_BINARY_DIR}/generated)
target_precompile_headers(construe_regen PRIVATE <nlohmann/json.hpp> <Eigen/Core> <Eigen/Geometry> <map> <set> <string> <vector>)
if(MSVC)
  target_compile_options(construe_regen PRIVATE /bigobj)  # regen.cpp — more than 2^16 sections (splitting — stage 2A)
endif()

# Recognizing the build history of an imported body (spec/reconstruction.md, S4-REC-002).
construe_add_module(rec
  SOURCES
    rec/recognize.cpp
    rec/rec_base.cpp
    rec/rec_cuts.cpp
    rec/rec_pattern.cpp
    rec/rec_text.cpp
    rec/rec_text_fit.cpp
    rec/rec_loft.cpp
    rec/rec_search.cpp
    rec/rec_dedup.cpp
    rec/rec_blend.cpp
  DEPS
    construe::base
    construe::font
    construe::kernel
    construe::regen
)

# Recognizing surfaces from a mesh and describing a body made of surfaces (spec/mesh-fit.md, S4-MESH-001).
construe_add_module(meshfit
  SOURCES
    meshfit/surface.cpp
    meshfit/segment.cpp
    meshfit/snap.cpp
    meshfit/topology.cpp
    meshfit/meshfit.cpp
  DEPS
    construe::base
    construe::kernel
    Eigen3::Eigen
)
if(MSVC)
  target_compile_options(construe_meshfit PRIVATE /bigobj)  # Eigen templates: more than 2^16 sections
endif()

# Drawings: sheets, sheet primitives, views, title block, dimensions (spec/drawing.md); specifications (spec/specification.md).
construe_add_module(drawing
  SOURCES
    drawing/sheet.cpp
    drawing/title_block.cpp
    drawing/title_block_iso.cpp
    drawing/standard.cpp
    drawing/views.cpp
    drawing/sheet_texts.cpp
    drawing/sheet_marks.cpp
    drawing/view_render.cpp
    drawing/sections.cpp
    drawing/view_decor.cpp
    drawing/render.cpp
    drawing/specification.cpp
    drawing/dimensions.cpp
    drawing/constructions.cpp
    drawing/model_dimensions.cpp
    drawing/view_pmi.cpp
    drawing/weld_annotations.cpp
    drawing/weld_geometry.cpp
    drawing/gear_view.cpp
    drawing/cut_plan.cpp
    drawing/sheet_parts.cpp
  DEPS
    construe::base
    construe::doc
    construe::kernel
    construe::regen
    construe::sheet
    construe::gear
    construe::font
    construe::pmi
)

construe_add_module(io
  SOURCES
    io/stl.cpp
    io/mesh_export.cpp
    io/sheet_export.cpp
    io/deflate.cpp
    io/dxf_writer.cpp
    io/sheet_dxf.cpp
    io/flat_dxf.cpp
    io/mesh_import.cpp
  DEPS
    construe::base
    construe::kernel
    construe::drawing
)

# Exchange with FreeCAD (stage 4A, spec/fcstd.md): reading .FCStd (own zip and XML) and importing into a document.
construe_add_module(fcstd
  SOURCES
    fcstd/xml.cpp
    fcstd/fcstd.cpp
    fcstd/brep_index.cpp
    fcstd/import.cpp
    fcstd/import_sketch.cpp
    fcstd/import_features.cpp
    fcstd/import_assembly.cpp
    fcstd/writer.cpp
    fcstd/export.cpp
    fcstd/export_sketch.cpp
    fcstd/export_features.cpp
    fcstd/export_assembly.cpp
  DEPS
    construe::base
    construe::expr
    construe::sketch
    construe::doc
    construe::kernel
    construe::regen
    construe::io
    construe::font
)

construe_add_module(api
  SOURCES
    api/session.cpp
    api/session_sketch.cpp
    api/session_assembly.cpp
    api/xref_commands.cpp
    api/variant_commands.cpp
    api/session_sections.cpp
    api/session_annotations.cpp
    api/session_welds.cpp
    api/session_repair.cpp
    api/session_patterns.cpp
    api/session_measure.cpp
    api/sections.cpp
    api/session_selection.cpp
    api/sketch_ops.cpp
    api/feature_commands.cpp
    api/sketch_commands.cpp
    api/sketch_groups.cpp
    api/sketch_relations.cpp
    api/context_commands.cpp
    api/export_commands.cpp
    api/step_commands.cpp
    api/mesh_commands.cpp
    api/fcstd_commands.cpp
    api/reconstruct_commands.cpp
    api/job_runner.cpp
    api/tess_cache.cpp
    api/regen_disk_cache.cpp
    api/projection_cache.cpp
    api/drawing_commands.cpp
    api/specification_commands.cpp
    api/drawing_standard_commands.cpp
    api/library_commands.cpp
    api/owned_cuts.cpp
    api/solution_commands.cpp
  DEPS
    construe::base
    construe::expr
    construe::sketch
    construe::doc
    construe::kernel
    construe::regen
    construe::io
    construe::drawing
    construe::pmi
    construe::rec
    construe::meshfit
    construe::library
    construe::solutions
    construe::fcstd
    construe::printprep
)
target_precompile_headers(construe_api PRIVATE <nlohmann/json.hpp> <Eigen/Core> <Eigen/Geometry> <map> <set> <string> <vector>)
target_compile_definitions(construe_api PRIVATE CONSTRUE_VERSION="${PROJECT_VERSION}")
if(MSVC)
  target_compile_options(construe_api PRIVATE /bigobj)  # session_assembly.cpp — more than 2^16 sections
endif()

# Local session factory: api + the core implementation on OCCT (architecture.md §2).
construe_add_module(api_local
  SOURCES
    api/local_session.cpp
  DEPS
    construe::api
  PRIVATE_DEPS
    construe::kernel_occt
)

# Qt application (docs/dev/client.md). The library construe_app_lib is used
# by the executable and by the e2e tests.
# Console import and history reconstruction (spec/reconstruction.md §12, FX10-REC-001): no Qt, rebuilt with the rec module.
construe_add_module(rec_console
  SOURCES
    rec_cli/rec_cli.cpp
  DEPS
    nlohmann_json::nlohmann_json
  PRIVATE_DEPS
    construe::api_local
)
add_executable(construe_rec_cli rec_cli/main.cpp)
target_link_libraries(construe_rec_cli PRIVATE construe::rec_console)

if(CONSTRUE_BUILD_APP)
  set(CMAKE_AUTOMOC ON)
  add_library(construe_app_lib STATIC
    app/theme.h
    app/gl_setup.h app/gl_setup.cpp
    app/messages_table.h
    app/messages.h app/messages.cpp
    app/retranslate.h app/retranslate.cpp
    app/captions.h app/captions.cpp
    app/problem_report.h app/problem_report.cpp
    app/crash_handler.h app/crash_handler.cpp
    app/session_log.h app/session_log.cpp
    app/action_tracker.h app/action_tracker.cpp
    app/foreground.h app/foreground.cpp
    app/window_placement.h app/window_placement.cpp
    app/file_open_events.h app/file_open_events.cpp
    app/long_operation.h app/long_operation.cpp
    app/busy_indicator.h app/busy_indicator.cpp
    app/ui_clock.h app/ui_clock.cpp
    app/help_content.h app/help_content.cpp
    app/help_window.h app/help_window.cpp
    app/report_window.h app/report_window.cpp
    app/update_check.h app/update_check.cpp
    app/portable.h app/portable.cpp
    app/update_window.h app/update_window.cpp
    app/camera.h app/camera.cpp
    app/snap.h app/snap.cpp
    app/sketch_geometry.h app/sketch_geometry.cpp
    app/sketch_annotations.h app/sketch_annotations.cpp
    app/sketch_picker.h app/sketch_picker.cpp
    app/inline_dimension_edit.h app/inline_dimension_edit.cpp
    app/expr_input.h app/expr_input.cpp app/expr_field.h app/expr_field.cpp app/dimension_field.h app/dimension_field.cpp app/tolerance_edit.h app/tolerance_edit.cpp
    app/scene_builder.h app/scene_builder.cpp
    app/mode_layout.h app/mode_layout.cpp
    app/command_availability.h app/command_availability.cpp
    app/part_tool.h app/part_tool.cpp
    app/selection_field.h app/selection_field.cpp app/selection_bubble.h app/selection_bubble.cpp app/main_window_selection.cpp
    app/selection_items.h app/selection_items.cpp app/selection_field_widget.h app/selection_field_widget.cpp app/slot_tree.h app/slot_tree.cpp
    app/repair_panel.h app/repair_panel.cpp app/main_window_repair.cpp
    app/toast_host.h app/toast_host.cpp
    app/conflict_panel.h app/conflict_panel.cpp
    app/export_panel.h app/export_panel.cpp
    app/import_panel.h app/import_panel.cpp
    app/library_panel.h app/library_panel.cpp
    app/solution_wizard.h app/solution_wizard.cpp
    app/solution_recorder.h app/solution_recorder.cpp
    app/settings_panel.h app/settings_panel.cpp
    app/app_settings.h app/app_settings.cpp
    app/app_settings_window.h app/app_settings_window.cpp
    app/properties_panel.h app/properties_panel.cpp
    app/section_panel.h app/section_panel.cpp
    app/measure_panel.h app/measure_panel.cpp
    app/pmi_panel.h app/pmi_panel.cpp app/pmi_overlay.h app/pmi_overlay.cpp app/tolerance_editor.h app/tolerance_editor.cpp
    app/main_window_pmi.cpp
    app/text_box.h app/text_box.cpp app/rich_text_field.h app/rich_text_field.cpp
    app/symbol_field.h app/symbol_field.cpp
    app/button_tips.h app/button_tips.cpp
    app/pattern_box.h app/pattern_box.cpp
    app/primitive_box.h app/primitive_box.cpp
    app/mate_panel.h app/mate_panel.cpp app/mate_logic.h app/mate_logic.cpp
    app/instance_pattern_panel.h app/instance_pattern_panel.cpp app/main_window_patterns.cpp
    app/weld_panel.h app/weld_panel.cpp app/main_window_welds.cpp
    app/document_hub.h
    app/element_clipboard.h app/element_clipboard.cpp app/main_window_xref.cpp
    app/viewport_types.h
    app/constraint_commands.h
    app/context_bar.h app/context_bar.cpp
    app/gl_renderer.h app/gl_renderer.cpp
    app/icons.h app/icons.cpp
    app/hotkeys_table.h
    app/viewport.h app/viewport.cpp
    app/about_window.h app/about_window.cpp
    app/fcstd_report.h app/fcstd_report.cpp
    app/app_model.h app/app_model.cpp
    app/sketch_editor.h app/sketch_editor.cpp app/sketch_editor_pattern.cpp app/sketch_editor_primitive.cpp app/sketch_editor_offset.cpp
    app/feature_tree.h app/feature_tree.cpp
    app/app_data.h app/app_data.cpp
    app/feature_panel.h app/feature_panel.cpp app/feature_panel_extrude.cpp app/feature_panel_dress.cpp app/feature_panel_ref.cpp app/feature_panel_sweep.cpp app/feature_panel_hole.cpp app/feature_panel_gear.cpp app/feature_panel_pattern.cpp app/feature_panel_sheet.h app/feature_panel_sheet.cpp
    app/variables_panel.h app/variables_panel.cpp
    app/panel_header.h app/panel_header.cpp
    app/popup_box.h app/popup_box.cpp app/dialog_buttons.h app/dialog_buttons.cpp app/check_list.h app/check_list.cpp app/path_list_edit.h app/path_list_edit.cpp app/flip_button.h app/flip_button.cpp app/choice_row.h app/choice_row.cpp
    app/title_block_form.h app/title_block_form.cpp
    app/main_window.h app/main_window.cpp app/main_window_ui.cpp app/main_window_ribbon.cpp app/main_window_assembly.cpp app/main_window_features.cpp app/main_window_import.cpp app/main_window_library.cpp app/main_window_solutions.cpp app/main_window_drawing.cpp app/main_window_spec.cpp app/drawing_view.h app/drawing_view.cpp app/drawing_dims.h app/drawing_dims.cpp app/drawing_texts.h app/drawing_texts.cpp app/drawing_marks.h app/drawing_marks.cpp app/sheet_print.h app/sheet_print.cpp app/main_window_drawing_extra.cpp app/drawing_constructions.h app/drawing_constructions.cpp
  )
  target_include_directories(construe_app_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
  # Build number and commit (spec/update.md §2, REL-CT-002): regenerated at every build; the header changes only when
  # the commit or the options change. CONSTRUE_BUILD_NUMBER — explicit number for builds without git (FX14-PKG-001).
  set(CONSTRUE_BUILD_NUMBER "" CACHE STRING "Номер сборки (пусто — время коммита HEAD в UTC, YYYYMMDDhhmm)")
  # Update system and package format (spec/update.md §2а, REL-CT-003): whose latest.json is read and how «Обновить»
  # updates. Windows builds are the installer content (setup); other source builds have no package (none).
  if(WIN32)
    set(_update_os windows)
    set(_package_format setup)
  elseif(APPLE)
    set(_update_os macos)
    set(_package_format none)
  else()
    set(_update_os linux)
    set(_package_format none)
  endif()
  set(CONSTRUE_UPDATE_OS "${_update_os}" CACHE STRING "Система обновлений: windows, linux, macos (spec/update.md §2а)")
  set(CONSTRUE_PACKAGE_FORMAT "${_package_format}" CACHE STRING "Формат пакета: setup, appimage, flatpak, dmg, none (spec/update.md §2а)")
  set_property(CACHE CONSTRUE_UPDATE_OS PROPERTY STRINGS windows linux macos)
  set_property(CACHE CONSTRUE_PACKAGE_FORMAT PROPERTY STRINGS setup appimage flatpak dmg none)
  set(_format_os_setup windows)
  set(_format_os_appimage linux)
  set(_format_os_flatpak linux)
  set(_format_os_dmg macos)
  if(NOT CONSTRUE_UPDATE_OS MATCHES "^(windows|linux|macos)$")
    message(FATAL_ERROR "CONSTRUE_UPDATE_OS=${CONSTRUE_UPDATE_OS}: допустимо windows, linux, macos (spec/update.md §2а)")
  endif()
  if(NOT CONSTRUE_PACKAGE_FORMAT MATCHES "^(setup|appimage|flatpak|dmg|none)$")
    message(FATAL_ERROR "CONSTRUE_PACKAGE_FORMAT=${CONSTRUE_PACKAGE_FORMAT}: допустимо setup, appimage, flatpak, dmg, none (spec/update.md §2а)")
  endif()
  if(NOT CONSTRUE_PACKAGE_FORMAT STREQUAL "none" AND NOT _format_os_${CONSTRUE_PACKAGE_FORMAT} STREQUAL CONSTRUE_UPDATE_OS)
    message(FATAL_ERROR "формат ${CONSTRUE_PACKAGE_FORMAT} не для системы ${CONSTRUE_UPDATE_OS} (spec/update.md §2а)")
  endif()
  set(_build_info ${CMAKE_BINARY_DIR}/generated/construe_build_info.h)
  add_custom_target(construe_build_info
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${PROJECT_SOURCE_DIR} -DOUTPUT=${_build_info} -DBUILD_NUMBER=${CONSTRUE_BUILD_NUMBER}
            -DUPDATE_OS=${CONSTRUE_UPDATE_OS} -DPACKAGE_FORMAT=${CONSTRUE_PACKAGE_FORMAT}
            -P ${PROJECT_SOURCE_DIR}/cmake/build_info.cmake
    BYPRODUCTS ${_build_info}
    COMMENT "Build number (cmake/build_info.cmake)"
    VERBATIM)
  add_dependencies(construe_app_lib construe_build_info)
  target_include_directories(construe_app_lib PUBLIC ${CMAKE_BINARY_DIR}/generated)
  # License identifier for «О программе» (app-ux §2.1а): read from LICENSE at configure time — the licensing model is
  # described only in LICENSE (docs/legal/README.md); a change of LICENSE re-runs the configuration.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/LICENSE)
  file(STRINGS ${PROJECT_SOURCE_DIR}/LICENSE CONSTRUE_LICENSE_SPDX_LINE REGEX "^SPDX-License-Identifier:" LIMIT_COUNT 1)
  string(REGEX REPLACE "^SPDX-License-Identifier:[ \t]*" "" CONSTRUE_LICENSE_SPDX "${CONSTRUE_LICENSE_SPDX_LINE}")
  if(CONSTRUE_LICENSE_SPDX STREQUAL "")
    message(FATAL_ERROR "LICENSE: no SPDX-License-Identifier line")
  endif()
  target_compile_definitions(construe_app_lib PRIVATE CONSTRUE_VERSION="${PROJECT_VERSION}" CONSTRUE_VERSION_PRERELEASE="${CONSTRUE_VERSION_PRERELEASE}" CONSTRUE_LICENSE_SPDX="${CONSTRUE_LICENSE_SPDX}")
  target_link_libraries(construe_app_lib PUBLIC
    construe::api_local
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets Qt6::Network Qt6::PrintSupport)
  # Problem report (app-ux §10.8): MiniDumpWriteDump on Windows.
  if(WIN32)
    target_link_libraries(construe_app_lib PUBLIC Dbghelp)
  endif()
  # Precompiled headers (stage 2A, build speed-up, docs/dev/setup.md): third-party and standard ones only —
  # they change rarely; project headers are not included.
  target_precompile_headers(construe_app_lib PRIVATE
    <QtWidgets> <QtGui> <QOpenGLFunctions> <nlohmann/json.hpp> <Eigen/Core> <Eigen/Geometry>
    <algorithm> <functional> <map> <memory> <optional> <set> <string> <vector>)
  # Application icon (S4-APP-001, app-ux §2.5): the picture — tools/gen_app_icon.py; PNGs of all sizes — a Qt resource
  # (the window icon), ICO — a resource of the Windows executable.
  set(CONSTRUE_ICON_SIZES 16 24 32 48 64 128 256)
  set(CONSTRUE_ICON_PNGS)
  foreach(sz ${CONSTRUE_ICON_SIZES})
    list(APPEND CONSTRUE_ICON_PNGS ${PROJECT_SOURCE_DIR}/assets/icons/construe-${sz}.png)
  endforeach()
  qt_add_resources(construe_app_lib "app_icons" PREFIX "/appicon" BASE "${PROJECT_SOURCE_DIR}/assets/icons" FILES ${CONSTRUE_ICON_PNGS})
  # The user guide with its screenshots for «Справка → Справка» (app-ux §2.1б, FX7-UI-007).
  file(GLOB_RECURSE CONSTRUE_HELP_IMAGES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/docs/user/images/*.png)
  qt_add_resources(construe_app_lib "help" PREFIX "/help" BASE "${PROJECT_SOURCE_DIR}/docs/user"
    FILES ${PROJECT_SOURCE_DIR}/docs/user/guide.ru.md ${PROJECT_SOURCE_DIR}/docs/user/guide.en.md ${CONSTRUE_HELP_IMAGES})
  # LICENSE and THIRD_PARTY_NOTICES for «О программе» (app-ux §2.1а, S7-LIC-003).
  qt_add_resources(construe_app_lib "legal" PREFIX "/legal" BASE "${PROJECT_SOURCE_DIR}"
    FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES)
  set(CONSTRUE_RC "")
  if(WIN32)
    set(CONSTRUE_RC ${CMAKE_CURRENT_BINARY_DIR}/construe.rc)
    file(CONFIGURE OUTPUT ${CONSTRUE_RC} CONTENT "IDI_APP ICON \"${PROJECT_SOURCE_DIR}/assets/icons/construe.ico\"\n")
  endif()
  add_executable(construe WIN32 app/main.cpp ${CONSTRUE_RC})
  target_link_libraries(construe PRIVATE construe_app_lib)
  set_target_properties(construe PROPERTIES WIN32_EXECUTABLE ON)
endif()
