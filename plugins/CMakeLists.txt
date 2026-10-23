# Airbound in-tree plugins. Built against the same plotjuggler_sdk the app pins
# (conanfile.txt), so the plugin ABI always matches the host it ships with.
#
# All plugin DLLs land in one config-independent folder so the app can be
# pointed at it (Preferences > plugin folders, or --plugin-dir).
set(AIRBOUND_PLUGINS_OUTPUT_DIR "${CMAKE_BINARY_DIR}/airbound_plugins")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "$<1:${AIRBOUND_PLUGINS_OUTPUT_DIR}>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "$<1:${AIRBOUND_PLUGINS_OUTPUT_DIR}>")

add_subdirectory(data_load_apbin)
