include(Catch)

add_executable(nmxd_tests
    test_bt_provider.cpp
    test_budgets.cpp
    test_config.cpp
    test_cli.cpp
    test_golden.cpp
    test_hash.cpp
    test_match.cpp
    test_json_provider.cpp
    test_jobs.cpp
    test_layout.cpp
    test_result.cpp
    test_session.cpp
    test_source.cpp
    test_textdiff.cpp
    test_tree.cpp
    test_xml_provider.cpp
)

target_link_libraries(nmxd_tests
    PRIVATE
        nmxd::core
        nmxd::cli
        nmxd_warnings
        Catch2::Catch2WithMain
)

target_compile_definitions(nmxd_tests PRIVATE
    NMXD_TESTDATA_DIR="${CMAKE_SOURCE_DIR}/testdata"
)

catch_discover_tests(nmxd_tests)
