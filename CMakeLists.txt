cmake_minimum_required(VERSION 3.25)

project(nmxmldiff
    VERSION 0.1.0
    DESCRIPTION "Diff tree-shaped data in XML and JSON"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(NMXD_BUILD_GUI "Build the GUI application" ON)
option(NMXD_BUILD_TESTS "Build the tests" ON)
option(NMXD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

include(cmake/Dependencies.cmake)

# Warning settings shared by our own targets and not imposed on dependencies.
add_library(nmxd_warnings INTERFACE)
if(MSVC)
    target_compile_options(nmxd_warnings INTERFACE /W4 /permissive- /utf-8)
    if(NMXD_WARNINGS_AS_ERRORS)
        target_compile_options(nmxd_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(nmxd_warnings INTERFACE -Wall -Wextra -Wpedantic)
    if(NMXD_WARNINGS_AS_ERRORS)
        target_compile_options(nmxd_warnings INTERFACE -Werror)
    endif()
endif()

add_subdirectory(src/core)
add_subdirectory(src/app)

if(NMXD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "nmxmldiff ${PROJECT_VERSION}: gui=${NMXD_BUILD_GUI} tests=${NMXD_BUILD_TESTS}")
