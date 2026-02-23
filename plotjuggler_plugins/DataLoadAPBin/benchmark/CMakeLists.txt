add_executable(apbin_benchmark main.cpp)

# Force console subsystem so stdout/stderr are attached when linking Qt5::Widgets.
# Without this MSVC defaults to /SUBSYSTEM:WINDOWS and printf goes nowhere.
if(MSVC)
    target_link_options(apbin_benchmark PRIVATE /SUBSYSTEM:CONSOLE)
endif()

target_include_directories(apbin_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/plotjuggler_base/include)

target_link_libraries(apbin_benchmark PRIVATE
    Qt5::Core
    Qt5::Widgets
    Qt5::Xml
    plotjuggler_base)

# Force console subsystem so stdout is attached even though Qt5::Widgets
# would otherwise push /SUBSYSTEM:WINDOWS.
if(MSVC)
    set_target_properties(apbin_benchmark PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:CONSOLE")
endif()

# On Windows, copy the base DLL next to the benchmark exe so it runs
# without needing the full install step.
if(WIN32)
    add_custom_command(TARGET apbin_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:plotjuggler_base>
            $<TARGET_FILE_DIR:apbin_benchmark>)
endif()
