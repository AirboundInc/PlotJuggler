add_executable(apbin_benchmark main.cpp)

target_include_directories(apbin_benchmark PRIVATE
    ${CMAKE_SOURCE_DIR}/plotjuggler_base/include)

target_link_libraries(apbin_benchmark PRIVATE
    Qt5::Core
    Qt5::Widgets
    Qt5::Xml
    plotjuggler_base)

# On Windows, copy the Qt + base DLLs next to the benchmark exe so it runs
# without needing the full install step.
if(WIN32)
    add_custom_command(TARGET apbin_benchmark POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:plotjuggler_base>
            $<TARGET_FILE_DIR:apbin_benchmark>)
endif()
