# The command line surface and the entry point. The GUI is a separate library
# so that the headless path, and the tests, never link a window toolkit.

add_library(nmtreediff_cli STATIC
    cli.cpp
    configure.cpp
    console.cpp
    report.cpp
)
target_include_directories(nmtreediff_cli PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(nmtreediff_cli
    PUBLIC nmtreediff::core
    PRIVATE CLI11::CLI11 nmtreediff_warnings
)
target_compile_definitions(nmtreediff_cli PRIVATE NMTREEDIFF_VERSION="${PROJECT_VERSION}")
add_library(nmtreediff::cli ALIAS nmtreediff_cli)

# The copyright line, which the About dialog and the version resource both
# state and both take from here.
set(NMTREEDIFF_COPYRIGHT "Copyright (c) 2026 Jan Milík")

if(NMTREEDIFF_BUILD_GUI)
    add_library(nmtreediff_ui STATIC
        ${CMAKE_SOURCE_DIR}/src/ui/about.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/app_window.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/file_picker.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/node_view.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/screenshot.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/text_view.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/theme.cpp
        ${CMAKE_SOURCE_DIR}/src/ui/welcome.cpp
    )
    target_include_directories(nmtreediff_ui PUBLIC ${CMAKE_SOURCE_DIR}/src)

    # What the About dialog says about this build, as a generated header. It
    # goes under a directory of its own so that it is included as
    # "ui/about_info.h", the way the headers beside its template are.
    set(NMTREEDIFF_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
    configure_file(
        ${CMAKE_SOURCE_DIR}/src/ui/about_info.h.in
        ${NMTREEDIFF_GENERATED_DIR}/ui/about_info.h
        @ONLY
    )
    target_include_directories(nmtreediff_ui PRIVATE ${NMTREEDIFF_GENERATED_DIR})

    target_link_libraries(nmtreediff_ui
        PUBLIC nmtreediff::core nmtreediff::cli
        PRIVATE imgui::imgui nfd::nfd nmtreediff_warnings
    )
    add_library(nmtreediff::ui ALIAS nmtreediff_ui)
endif()

add_executable(nmtreediff main.cpp)
target_link_libraries(nmtreediff PRIVATE nmtreediff::cli nmtreediff_warnings)

if(NMTREEDIFF_BUILD_GUI)
    target_link_libraries(nmtreediff PRIVATE nmtreediff::ui)
    target_compile_definitions(nmtreediff PRIVATE NMTREEDIFF_HAVE_GUI=1)
else()
    target_compile_definitions(nmtreediff PRIVATE NMTREEDIFF_HAVE_GUI=0)
endif()

# A GUI subsystem binary, so a launch from the desktop or a version control
# client opens no console window. That would lose stdout, which the headless
# path and every integration guide depend on, so the first thing main() does is
# find it again: a redirected handle is used as it is, and otherwise the
# parent's console is attached. See src/app/console.cpp. The entry point stays
# main(); only the subsystem changes.
set_target_properties(nmtreediff PROPERTIES WIN32_EXECUTABLE ON)
if(MSVC)
    target_link_options(nmtreediff PRIVATE /ENTRY:mainCRTStartup)
endif()

# The version resource, which is what the file's properties, Task Manager and
# installers read to say what the executable is. The binary form of a version
# always has four parts, and project() leaves the ones it was not given empty,
# so those are filled with zeros.
if(WIN32)
    set(NMTREEDIFF_RC_VERSION_PARTS "")
    foreach(part IN ITEMS MAJOR MINOR PATCH TWEAK)
        if(PROJECT_VERSION_${part} STREQUAL "")
            list(APPEND NMTREEDIFF_RC_VERSION_PARTS 0)
        else()
            list(APPEND NMTREEDIFF_RC_VERSION_PARTS ${PROJECT_VERSION_${part}})
        endif()
    endforeach()
    list(JOIN NMTREEDIFF_RC_VERSION_PARTS "," NMTREEDIFF_RC_VERSION)

    configure_file(version.rc.in ${CMAKE_CURRENT_BINARY_DIR}/version.rc @ONLY)
    target_sources(nmtreediff PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/version.rc)
endif()

# The application icon, embedded as a resource. The resource compiler does not
# report the icon as a dependency of the script, so it is named here; without
# that a regenerated icon would not be picked up.
if(WIN32)
    set(NMTREEDIFF_ICON_DIR ${CMAKE_SOURCE_DIR}/assets/icon)
    target_sources(nmtreediff PRIVATE nmtreediff.rc)
    set_source_files_properties(nmtreediff.rc PROPERTIES
        INCLUDE_DIRECTORIES ${NMTREEDIFF_ICON_DIR}
        OBJECT_DEPENDS ${NMTREEDIFF_ICON_DIR}/nmtreediff.ico
    )
endif()
