include(cmake/sources.cmake)

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(asio
    URL https://github.com/chriskohlhoff/asio/archive/refs/tags/asio-1-36-0.tar.gz
)
FetchContent_MakeAvailable(asio)
find_package(Threads REQUIRED)
include(cmake/sqlite.cmake)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)

add_library(sos-asio INTERFACE)
target_include_directories(sos-asio SYSTEM INTERFACE "${asio_SOURCE_DIR}/asio/include")
target_compile_definitions(sos-asio INTERFACE ASIO_STANDALONE)
target_compile_features(sos-asio INTERFACE cxx_std_23)
target_link_libraries(sos-asio INTERFACE Threads::Threads)

add_library(daemon-transport STATIC ${DAEMON_TRANSPORT_SOURCES})
target_include_directories(daemon-transport PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(daemon-transport PUBLIC sos-asio nlohmann_json::nlohmann_json)

add_library(daemon-sessions STATIC ${DAEMON_SESSION_SOURCES})
target_link_libraries(daemon-sessions PUBLIC daemon-transport sos::agent-runtime)
target_link_libraries(daemon-sessions PRIVATE OpenSSL::Crypto sos-sqlite)

add_executable(sosd ${DAEMON_SOURCES})
target_link_libraries(sosd PRIVATE daemon-sessions)
