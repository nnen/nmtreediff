# Dependencies are fetched and pinned by exact ref rather than resolved from a
# package manager. The tool has few dependencies, and this way a contributor
# needs nothing installed beyond CMake, Ninja and a compiler.

include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------- CLI11 -----
FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        6c7b07a878ad834957b98d0f9ce1dbe0cb204fc9  # v2.4.2
    GIT_SHALLOW    TRUE
)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_DOCS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(cli11)

# -------------------------------------------------------------- pugixml -----
FetchContent_Declare(
    pugixml
    GIT_REPOSITORY https://github.com/zeux/pugixml.git
    GIT_TAG        ee86beb30e4973f5feffe3ce63bfa4fbadf72f38  # v1.15
    GIT_SHALLOW    TRUE
)
set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(pugixml)

# ------------------------------------------------------------- simdjson -----
# Chosen for its source locations rather than its throughput: a node span is
# what links the node view to the text view, and the On Demand API is the only
# widely used JSON parser that reports where in the bytes each value sat.
FetchContent_Declare(
    simdjson
    GIT_REPOSITORY https://github.com/simdjson/simdjson.git
    GIT_TAG        0c0ce1bd48baa0677dc7c0945ea7cd1e8b52b297  # v3.13.0
    GIT_SHALLOW    TRUE
)
set(SIMDJSON_DEVELOPER_MODE OFF CACHE BOOL "" FORCE)
set(SIMDJSON_ENABLE_THREADS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(simdjson)

# simdjson exports its include directory as an ordinary one, so its headers are
# compiled under this project warning settings and trip them. Re-exporting the
# same directory as a system include silences that without relaxing anything
# that applies to code written here.
get_target_property(_simdjson_includes simdjson INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(simdjson PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_simdjson_includes}")

# ----------------------------------------------------------------- GUI ------
if(NMTREEDIFF_BUILD_GUI)
    find_package(OpenGL REQUIRED)

    FetchContent_Declare(
        glfw
        GIT_REPOSITORY https://github.com/glfw/glfw.git
        GIT_TAG        a74efa0d5628b74adc0426af4c5710e287fa7c2c  # 3.4
        GIT_SHALLOW    TRUE
    )
    set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(glfw)

    # Dear ImGui ships no CMake of its own, so the sources are compiled here.
    # Pinned to a commit on the docking branch, which is not a release branch;
    # upgrade deliberately rather than tracking the tip.
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        7e1b65d26d52e9dd199d889c148c72184de647b4  # docking
        GIT_SHALLOW    FALSE
    )
    FetchContent_MakeAvailable(imgui)

    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    )
    target_include_directories(imgui SYSTEM PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(imgui PUBLIC glfw OpenGL::GL)
    target_compile_features(imgui PUBLIC cxx_std_20)
    add_library(imgui::imgui ALIAS imgui)
endif()

# ------------------------------------------------------------------ Lua -----
# Configuration files are Lua scripts, and so are format providers. Upstream
# Lua ships a makefile rather than a CMake build, so the sources are compiled
# here the same way Dear ImGui's are. Three are left out: lua.c is the
# interpreter's command-line front end, and this embeds a library instead;
# ltests.c is the test harness; and onelua.c is an amalgamation of every other
# file, which would define the whole library a second time.
FetchContent_Declare(
    lua
    GIT_REPOSITORY https://github.com/lua/lua.git
    GIT_TAG        1ab3208a1fceb12fca8f24ba57d6e13c5bff15e3  # v5.4.7
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(lua)

file(GLOB _lua_sources ${lua_SOURCE_DIR}/*.c)
list(FILTER _lua_sources EXCLUDE REGEX "/(lua|luac|onelua|ltests)\\.c$")
add_library(lua STATIC ${_lua_sources})
# Compiled as C++ so that a script error unwinds as an exception rather than
# through longjmp, which would step over the destructors of everything the
# bridge holds while a callback is running.
set_source_files_properties(${_lua_sources} PROPERTIES LANGUAGE CXX)
target_include_directories(lua SYSTEM PUBLIC ${lua_SOURCE_DIR})
# Compiling Lua as C++ means its symbols are C++ symbols. sol2 assumes a C build
# and wraps its includes in extern "C" unless told otherwise, which would leave
# every one of them unresolved at link time.
target_compile_definitions(lua PUBLIC SOL_USING_CXX_LUA=1)
add_library(lua::lua ALIAS lua)

# ----------------------------------------------------------------- sol2 -----
# Header-only, and the reason for choosing it over the raw C API: it binds a
# C++ class to a Lua table without a code generator, which is what the provider
# bridge needs and what hand-rolling would make expensive.
FetchContent_Declare(
    sol2
    GIT_REPOSITORY https://github.com/ThePhD/sol2.git
    GIT_TAG        dca62a0f02bb45f3de296de3ce00b1275eb34c25  # v3.3.1
    GIT_SHALLOW    TRUE
)
set(SOL2_BUILD_LUA OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(sol2)

# --------------------------------------------- nativefiledialog-extended -----
# Dear ImGui has no file dialog, and a diff tool that cannot open a file from
# inside its own window is not one a person can launch. A native dialog is what
# an artist expects: recent places, a typed network path, and the shell's own
# sorting. Writing one per platform is three backends and a COM apartment; this
# is one small MIT dependency with a CMake build.
if(NMTREEDIFF_BUILD_GUI)
    FetchContent_Declare(
        nfd
        GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
        GIT_TAG        86d5f2005fe1c00747348a12070fec493ea2407e  # v1.2.1
        GIT_SHALLOW    TRUE
    )
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(nfd)
endif()

# --------------------------------------------------------------- Catch2 -----
if(NMTREEDIFF_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        fa43b77429ba76c462b1898d6cd2f2d7a9416b14  # v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
endif()
