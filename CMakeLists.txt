cmake_minimum_required(VERSION 3.25)

project(nmtreediff
    VERSION 0.3.2
    DESCRIPTION "Diff tree-shaped data in XML and JSON"
    HOMEPAGE_URL "https://github.com/nnen/nmtreediff"
    LANGUAGES CXX
)

# The resource script that embeds the application icon; see src/app.
if(WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# The C and C++ runtime is linked statically under the Microsoft compiler, so
# the executable runs on a machine that has no Visual C++ redistributable
# installed and can be shipped as a single file. Every library in one program
# must agree on its runtime, so this is set before the dependencies are added
# and they inherit it.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(NMTREEDIFF_BUILD_GUI "Build the GUI application" ON)
option(NMTREEDIFF_BUILD_TESTS "Build the tests" ON)
option(NMTREEDIFF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/Dependencies.cmake)

# Warning settings shared by our own targets and not imposed on dependencies.
add_library(nmtreediff_warnings INTERFACE)
if(MSVC)
    target_compile_options(nmtreediff_warnings INTERFACE /W4 /permissive- /utf-8)
    if(NMTREEDIFF_WARNINGS_AS_ERRORS)
        target_compile_options(nmtreediff_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(nmtreediff_warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(NMTREEDIFF_WARNINGS_AS_ERRORS)
        target_compile_options(nmtreediff_warnings INTERFACE -Werror)
    endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

include(cmake/Packaging.cmake)

if(NMTREEDIFF_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# The documentation requirement is enforced rather than merely stated: the
# Doxyfile treats an undocumented entity as an error, so this target fails when
# something new arrives without documentation.
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
    add_custom_target(docs
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_SOURCE_DIR}/docs/api
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Generating the API reference with Doxygen"
        VERBATIM
    )
else()
    message(STATUS "Doxygen not found; the 'docs' target is unavailable")
endif()

message(STATUS "nmtreediff ${PROJECT_VERSION}: gui=${NMTREEDIFF_BUILD_GUI} tests=${NMTREEDIFF_BUILD_TESTS}")
