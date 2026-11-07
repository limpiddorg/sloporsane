include(cmake/sources.cmake)

find_package(GTest 1.17.0 QUIET)
if(NOT GTest_FOUND)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
    )
    set(INSTALL_GTEST OFF CACHE BOOL "Install GoogleTest" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

add_executable(agent-runtime-tests ${AGENT_RUNTIME_TEST_SOURCES})
target_link_libraries(agent-runtime-tests PRIVATE sos::agent-runtime GTest::gtest_main)
target_link_libraries(agent-runtime-tests PRIVATE ixwebsocket::ixwebsocket)

include(GoogleTest)
gtest_discover_tests(agent-runtime-tests)

add_executable(daemon-transport-tests ${DAEMON_TRANSPORT_TEST_SOURCES})
target_link_libraries(daemon-transport-tests PRIVATE daemon-transport GTest::gtest_main)
gtest_discover_tests(daemon-transport-tests)

add_executable(daemon-session-tests ${DAEMON_SESSION_TEST_SOURCES})
target_link_libraries(daemon-session-tests PRIVATE daemon-sessions GTest::gtest_main)
gtest_discover_tests(daemon-session-tests)

add_executable(session-fixture session_fixture.cpp)
target_link_libraries(session-fixture PRIVATE daemon-sessions)
