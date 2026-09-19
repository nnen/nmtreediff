include(Catch)

add_executable(nmtreediff_tests
    test_bt_provider.cpp
    test_builder.cpp
    test_dom.cpp
    test_shape.cpp
    test_budgets.cpp
    test_config.cpp
    test_cli.cpp
    test_golden.cpp
    test_hash.cpp
    test_match.cpp
    test_report.cpp
    test_json_provider.cpp
    test_jobs.cpp
    test_direction.cpp
    test_file_filters.cpp
    test_layout.cpp
    test_log.cpp
    test_lua_provider.cpp
    test_result.cpp
    test_session.cpp
    test_source.cpp
    test_textdiff.cpp
    test_tree.cpp
    test_xml_provider.cpp
)

target_link_libraries(nmtreediff_tests
    PRIVATE
        nmtreediff::core
        nmtreediff::cli
        nmtreediff_warnings
        Catch2::Catch2WithMain
)

target_compile_definitions(nmtreediff_tests PRIVATE
    NMTREEDIFF_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata"
)

# The budget tests measure time, so they are registered as their own labelled
# suite: `ctest -L budget` runs them alone and `ctest -LE budget` runs
# everything else. Both are registered, and both run by default. They once
# carried Catch2's hidden tag, which kept them out of ctest altogether, and
# the numbers in the plan went unenforced for as long as it did.
catch_discover_tests(nmtreediff_tests TEST_SPEC "~[budget]")
catch_discover_tests(nmtreediff_tests TEST_SPEC "[budget]" PROPERTIES LABELS budget)
