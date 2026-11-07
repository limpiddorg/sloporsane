include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
FetchContent_Declare(tree_sitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.25.10.tar.gz
    SOURCE_SUBDIR unused
)
FetchContent_Declare(tree_sitter_bash
    URL https://github.com/tree-sitter/tree-sitter-bash/archive/refs/tags/v0.25.0.tar.gz
    SOURCE_SUBDIR unused
)
FetchContent_MakeAvailable(tree_sitter tree_sitter_bash)

add_library(sos-tree-sitter STATIC "${tree_sitter_SOURCE_DIR}/lib/src/lib.c")
target_include_directories(sos-tree-sitter PUBLIC "${tree_sitter_SOURCE_DIR}/lib/include"
    PRIVATE "${tree_sitter_SOURCE_DIR}/lib/src")
set_target_properties(sos-tree-sitter PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)

add_library(sos-tree-sitter-bash STATIC
    "${tree_sitter_bash_SOURCE_DIR}/src/parser.c"
    "${tree_sitter_bash_SOURCE_DIR}/src/scanner.c")
target_include_directories(sos-tree-sitter-bash PRIVATE "${tree_sitter_bash_SOURCE_DIR}/src")
set_target_properties(sos-tree-sitter-bash PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
