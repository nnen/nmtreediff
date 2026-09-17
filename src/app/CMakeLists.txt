# The command line surface and the entry point. The GUI is a separate library
# so that the headless path, and the tests, never link a window toolkit.

add_library(nmxd_cli STATIC
    cli.cpp
    configure.cpp
    console.cpp
    report.cpp
)
target_include_directories(nmxd_cli PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nmxd_cli
    PUBLIC nmxd::core
    PRIVATE CLI11::CLI11 nmxd_warnings
)
target_compile_definitions(nmxd_cli PRIVATE NMXD_VERSION="${PROJECT_VERSION}")
add_library(nmxd::cli ALIAS nmxd_cli)

if(NMXD_BUILD_GUI)
    add_library(nmxd_ui STATIC
        ${CMAKE_SOURCE_DIR}/src/ui/app_window.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/file_picker.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/node_view.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/screenshot.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/text_view.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/welcome.cpp
    )
    target_include_directories(nmxd_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(nmxd_ui
        PUBLIC nmxd::core nmxd::cli
        PRIVATE imgui::imgui nfd::nfd nmxd_warnings
    )
    add_library(nmxd::ui ALIAS nmxd_ui)
endif()

add_executable(nmxmldiff main.cpp)
target_link_libraries(nmxmldiff PRIVATE nmxd::cli nmxd_warnings)

if(NMXD_BUILD_GUI)
    target_link_libraries(nmxmldiff PRIVATE nmxd::ui)
    target_compile_definitions(nmxmldiff PRIVATE NMXD_HAVE_GUI=1)
else()
    target_compile_definitions(nmxmldiff PRIVATE NMXD_HAVE_GUI=0)
endif()

# A GUI subsystem binary, so a launch from the desktop or a version control
# client opens no console window. That would lose stdout, which the headless
# path and every integration guide depend on, so the first thing main() does is
# find it again: a redirected handle is used as it is, and otherwise the
# parent's console is attached. See src/app/console.cpp. The entry point stays
# main(); only the subsystem changes.
set_target_properties(nmxmldiff PROPERTIES WIN32_EXECUTABLE ON)
if(MSVC)
    target_link_options(nmxmldiff PRIVATE /ENTRY:mainCRTStartup)
endif()
