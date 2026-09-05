# The core library links against no GUI target. That is what keeps the engine
# testable in continuous integration and reusable by a future merge tool.

add_library(nmxd_core STATIC
    diff.cpp
    hash.cpp
    match.cpp
    jobs.cpp
    provider.cpp
    registry.cpp
    tree.cpp
    ${CMAKE_SOURCE_DIR}/src/formats/xml_generic.cpp
    session.cpp
    source.cpp
    textdiff.cpp
)

target_include_directories(nmxd_core PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_compile_features(nmxd_core PUBLIC cxx_std_20)
target_link_libraries(nmxd_core PRIVATE nmxd_warnings pugixml::pugixml)

find_package(Threads REQUIRED)
target_link_libraries(nmxd_core PUBLIC Threads::Threads)

add_library(nmxd::core ALIAS nmxd_core)
