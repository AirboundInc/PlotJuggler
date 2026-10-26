# Airbound in-tree plugins. Built against the same plotjuggler_sdk the app pins
# (conanfile.txt), so the plugin ABI always matches the host it ships with.
#
# All plugin DLLs land in one config-independent folder so the app can be
# pointed at it (Preferences > plugin folders, or --plugin-dir).
#
# Also buildable on its own (the plugin release workflow does this, see
# registry/README.md): configure this folder directly against a Conan toolchain
# that provides plotjuggler_sdk and nlohmann_json at the app's pins.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.22)
    project(airbound_plugins LANGUAGES CXX)
    find_package(plotjuggler_sdk CONFIG REQUIRED)
endif()

set(AIRBOUND_PLUGINS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/airbound_plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$<1:${AIRBOUND_PLUGINS_OUTPUT_DIR}>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "$<1:${AIRBOUND_PLUGINS_OUTPUT_DIR}>")

add_subdirectory(data_load_apbin)
