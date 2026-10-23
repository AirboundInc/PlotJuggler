# ArduPilot DataFlash (.bin) loader. LGPL-3.0 (see LICENSE in this folder).
# Ported from plotjuggler_plugins/DataLoadAPBin on Airbound's PJ3 fork
# (originally https://github.com/ArduPilot/plotjuggler-apbin-plugins).

find_package(nlohmann_json REQUIRED)

add_library(apbin_source_plugin SHARED
    apbin_source.cpp
    logformat.h)
target_compile_features(apbin_source_plugin PRIVATE cxx_std_20)
target_link_libraries(apbin_source_plugin PRIVATE
    plotjuggler_sdk::plugin_sdk
    nlohmann_json::nlohmann_json)
target_include_directories(apbin_source_plugin PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Optional: uncomment to enable units display in field names (as in PJ3)
# target_compile_definitions(apbin_source_plugin PRIVATE LABEL_WITH_UNIT)

pj_configure_plugin(apbin_source_plugin
    FAMILIES        data_source
    MANIFEST_FILE   ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
    MANIFEST_HEADER ${CMAKE_CURRENT_BINARY_DIR}/generated/apbin_manifest.hpp
    MANIFEST_VAR    kApbinManifest
)

# Dev tool: headless load + canonical dump (see tools/apbin_dump.cpp). Not part of the app.
add_executable(apbin_dump tools/apbin_dump.cpp)
target_compile_features(apbin_dump PRIVATE cxx_std_20)
target_link_libraries(apbin_dump PRIVATE plotjuggler_sdk::plugin_sdk nlohmann_json::nlohmann_json)
# Keep it out of the plugin folder the app scans.
set_target_properties(apbin_dump PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/airbound_tools>")

# `build.bat --minimal` only builds pj_app; make it build this plugin too.
if(TARGET pj_app)
    add_dependencies(pj_app apbin_source_plugin)
endif()
