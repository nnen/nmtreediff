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

# ----------------------------------------------------------------- GUI ------
if(NMXD_BUILD_GUI)
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
        GIT_SHALLOW    TRUE
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

# --------------------------------------------------------------- Catch2 -----
if(NMXD_BUILD_TESTS)
    FetchContent_Declare(
        Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG        fa43b77429ba76c462b1898d6cd2f2d7a9416b14  # v3.7.1
        GIT_SHALLOW    TRUE
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${Catch2_SOURCE_DIR}/extras)
endif()
