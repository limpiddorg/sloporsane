cmake_minimum_required(VERSION 3.20)

project(sloporsane LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(APPLE)
    find_program(SOS_HOMEBREW_EXECUTABLE brew PATHS /opt/homebrew/bin /usr/local/bin)
    if(SOS_HOMEBREW_EXECUTABLE)
        execute_process(COMMAND "${SOS_HOMEBREW_EXECUTABLE}" --prefix
            OUTPUT_VARIABLE SOS_HOMEBREW_PREFIX
            OUTPUT_STRIP_TRAILING_WHITESPACE
            COMMAND_ERROR_IS_FATAL ANY)
        list(PREPEND CMAKE_PREFIX_PATH "${SOS_HOMEBREW_PREFIX}")
        # Override cached Conda paths whose yaml-cpp includes can shadow Homebrew JSON headers.
        set(nlohmann_json_DIR
            "${SOS_HOMEBREW_PREFIX}/opt/nlohmann-json/share/cmake/nlohmann_json"
            CACHE PATH "Homebrew JSON package directory" FORCE)
        set(yaml-cpp_DIR
            "${SOS_HOMEBREW_PREFIX}/opt/yaml-cpp/lib/cmake/yaml-cpp"
            CACHE PATH "Homebrew yaml-cpp package directory" FORCE)
    endif()
endif()

find_package(nlohmann_json 3.12.0 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    )
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_subdirectory(src/agent-runtime)
add_subdirectory(src/daemon)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

option(SOS_BUILD_TEXTUAL "Build and install the Python/Textual integration" ON)
option(SOS_BUILD_FTXUI "Build the native C++/FTXUI integration" ON)
if(SOS_BUILD_TEXTUAL)
    add_subdirectory(cmake/build-tui)
endif()
if(SOS_BUILD_FTXUI)
    add_subdirectory(integrations/ftxui)
endif()
