# The core library links against no GUI target. That is what keeps the engine
# testable in continuous integration and reusable by a future merge tool.

add_library(nmtreediff_core STATIC
    builder.cpp
    dom.cpp
    shape.cpp
    config.cpp
    lua_config.cpp
    lua_provider.cpp
    lua_shape.cpp
    lua_state.cpp
    diff.cpp
    file_filters.cpp
    layout_tree.cpp
    hash.cpp
    match.cpp
    jobs.cpp
    log.cpp
    provider.cpp
    registry.cpp
    tree.cpp
    ${CMAKE_SOURCE_DIR}/src/formats/bt_xml.cpp
    ${CMAKE_SOURCE_DIR}/src/formats/json_generic.cpp
    ${CMAKE_SOURCE_DIR}/src/formats/xml_generic.cpp
    ${CMAKE_SOURCE_DIR}/src/formats/xml_spans.cpp
    session.cpp
    source.cpp
    textdiff.cpp
)

target_include_directories(nmtreediff_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(nmtreediff_core PUBLIC cxx_std_20)
target_link_libraries(nmtreediff_core
    PRIVATE nmtreediff_warnings pugixml::pugixml simdjson::simdjson lua::lua sol2::sol2
)

# sol2 instantiates enough templates that an unoptimised build of the Lua
# bridge exceeds the section count of the default object file format.
if(MSVC)
    target_compile_options(nmtreediff_core PRIVATE /bigobj)
endif()

find_package(Threads REQUIRED)
target_link_libraries(nmtreediff_core PUBLIC Threads::Threads)

add_library(nmtreediff::core ALIAS nmtreediff_core)
