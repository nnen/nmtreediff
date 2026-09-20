# The notices of the libraries compiled into the executable, gathered into one
# file that ships with every release. Included by Packaging.cmake, whose
# install rules put the file next to the licence.
#
# Most of these libraries ask that their copyright notice and licence text
# accompany a binary built from them, and naming them in the About dialog does
# not do that. The file is assembled here, at configure time, from the licence
# each pinned source tree carries, so that pinning a new version in
# Dependencies.cmake brings its notice along and there is no second copy to
# forget. Where a text has to be dug out of a source file, not finding it fails
# the configure step, so a release cannot go out with a notice missing.
#
# The test framework is left out: it is not part of what ships.

set(NMTREEDIFF_NOTICES_FILE ${CMAKE_BINARY_DIR}/THIRD-PARTY-NOTICES.txt)

set(NMTREEDIFF_NOTICES_RULE
    "===============================================================================")

# Starts the file over with the paragraph that says what it is.
function(nmtreediff_notices_begin)
    file(WRITE ${NMTREEDIFF_NOTICES_FILE}
        "NM Tree Diff is built with the libraries below. Each is the work of its own\n"
        "authors and is used under the licence that follows its name. NM Tree Diff\n"
        "itself is released under the licence in the LICENSE file next to this one.\n")
endfunction()

# Adds one library: a ruled heading with its name and home page, then the text.
# Line endings are made uniform, because a source tree checked out on Windows
# may have either kind.
function(nmtreediff_notices_add name homepage text)
    string(REPLACE "\r\n" "\n" text "${text}")
    string(STRIP "${text}" text)
    file(APPEND ${NMTREEDIFF_NOTICES_FILE}
        "\n${NMTREEDIFF_NOTICES_RULE}\n${name}\n${homepage}\n${NMTREEDIFF_NOTICES_RULE}\n\n${text}\n")
endfunction()

# Adds a library whose source tree carries its licence as a file.
function(nmtreediff_notices_add_file name homepage licence_file)
    if(NOT EXISTS ${licence_file})
        message(FATAL_ERROR "The licence of ${name} is not at ${licence_file}")
    endif()
    file(READ ${licence_file} text)
    nmtreediff_notices_add("${name}" "${homepage}" "${text}")
endfunction()

# Adds Lua, which has no licence file: its notice is the comment that closes
# lua.h. The comment's frame of asterisks is taken off, leaving the text.
function(nmtreediff_notices_add_lua)
    file(READ ${lua_SOURCE_DIR}/lua.h header)
    string(REPLACE "\r\n" "\n" header "${header}")
    string(REGEX MATCH "\\* Copyright [^\n]*Lua\\.org.*DEALINGS IN THE SOFTWARE\\." text "${header}")
    if(text STREQUAL "")
        message(FATAL_ERROR "The licence of Lua is not where it was in ${lua_SOURCE_DIR}/lua.h")
    endif()
    string(REGEX REPLACE "\n\\* ?" "\n" text "\n${text}")
    nmtreediff_notices_add("Lua" "https://www.lua.org" "${text}")
endfunction()

# Adds the fonts Dear ImGui embeds, which have authors and notices of their
# own. Each is a comment line above the font's data, stating the licence and
# the copyright together.
function(nmtreediff_notices_add_imgui_fonts)
    file(STRINGS ${imgui_SOURCE_DIR}/imgui_draw.cpp lines
        REGEX "^// MIT [Ll]icense / Copyright")
    if(lines STREQUAL "")
        message(FATAL_ERROR
            "The notices of Dear ImGui's fonts are not where they were in "
            "${imgui_SOURCE_DIR}/imgui_draw.cpp")
    endif()
    set(text "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^// MIT [Ll]icense / " "" line "${line}")
        string(APPEND text "${line}\n")
    endforeach()
    string(APPEND text
        "\nThese fonts are embedded in Dear ImGui and released under the MIT licence,\n"
        "the text of which is given under Dear ImGui above.")
    nmtreediff_notices_add("Proggy fonts" "https://github.com/ocornut/proggyforever" "${text}")
endfunction()

# In alphabetical order, the one the About dialog lists them in. The libraries
# of the window are compiled in only when the GUI is built.
nmtreediff_notices_begin()
nmtreediff_notices_add_file("CLI11" "https://github.com/CLIUtils/CLI11"
    ${cli11_SOURCE_DIR}/LICENSE)
if(NMTREEDIFF_BUILD_GUI)
    nmtreediff_notices_add_file("Dear ImGui" "https://github.com/ocornut/imgui"
        ${imgui_SOURCE_DIR}/LICENSE.txt)
    nmtreediff_notices_add_imgui_fonts()
    nmtreediff_notices_add_file("GLFW" "https://www.glfw.org"
        ${glfw_SOURCE_DIR}/LICENSE.md)
endif()
nmtreediff_notices_add_lua()
if(NMTREEDIFF_BUILD_GUI)
    nmtreediff_notices_add_file("Native File Dialog Extended"
        "https://github.com/btzy/nativefiledialog-extended"
        ${nfd_SOURCE_DIR}/LICENSE)
endif()
nmtreediff_notices_add_file("pugixml" "https://pugixml.org"
    ${pugixml_SOURCE_DIR}/LICENSE.md)
# simdjson offers a choice of two licences; this is the one taken.
nmtreediff_notices_add_file("simdjson" "https://github.com/simdjson/simdjson"
    ${simdjson_SOURCE_DIR}/LICENSE-MIT)
nmtreediff_notices_add_file("sol2" "https://github.com/ThePhD/sol2"
    ${sol2_SOURCE_DIR}/LICENSE.txt)
