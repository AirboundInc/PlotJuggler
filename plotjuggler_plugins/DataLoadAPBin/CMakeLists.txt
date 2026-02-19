# ArduPilot APBin DataLoader Plugin
# Source: https://github.com/ArduPilot/plotjuggler-apbin-plugins

include_directories(../)

qt5_wrap_ui(APBIN_UI_SRC
    apbin_messages_dialog.ui)

add_library(DataLoadAPBin SHARED
    dataload_apbin.h
    dataload_apbin.cpp
    apbin_messages_dialog.h
    apbin_messages_dialog.cpp
    ${APBIN_UI_SRC})

target_link_libraries(DataLoadAPBin
    PRIVATE
    Qt5::Core
    Qt5::Widgets
    Qt5::Xml
    plotjuggler_base)

target_compile_definitions(DataLoadAPBin PRIVATE QT_PLUGIN)

# Optional: Uncomment to enable units display in field names
# target_compile_definitions(DataLoadAPBin PRIVATE LABEL_WITH_UNIT)

install(TARGETS DataLoadAPBin DESTINATION ${PJ_PLUGIN_INSTALL_DIRECTORY})

add_subdirectory(benchmark)
