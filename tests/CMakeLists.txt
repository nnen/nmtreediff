include(Catch)

add_executable(nmxd_tests
    test_budgets.cpp
    test_cli.cpp
    test_hash.cpp
    test_jobs.cpp
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

catch_discover_tests(nmxd_tests)
