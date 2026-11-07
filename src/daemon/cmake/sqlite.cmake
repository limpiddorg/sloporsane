# SQLite is public domain. Pin the amalgamation and link it into the daemon;
# the installed application does not need a system SQLite library or CLI.
include(FetchContent)
FetchContent_Declare(sqlite
    URL https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
)
FetchContent_MakeAvailable(sqlite)
add_library(sos-sqlite STATIC "${sqlite_SOURCE_DIR}/sqlite3.c")
target_include_directories(sos-sqlite SYSTEM PUBLIC "${sqlite_SOURCE_DIR}")
target_compile_definitions(sos-sqlite PRIVATE SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_OMIT_LOAD_EXTENSION)
target_link_libraries(sos-sqlite PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
