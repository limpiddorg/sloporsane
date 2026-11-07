# cmark-gfm supplies the native CommonMark/GFM parser. Rendering remains C++/FTXUI.
set(CMARK_TESTS OFF CACHE BOOL "" FORCE)
set(CMARK_SHARED OFF CACHE BOOL "" FORCE)
set(CMARK_STATIC ON CACHE BOOL "" FORCE)
set(CMARK_LIB_FUZZER OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.10)
FetchContent_Declare(cmark_gfm URL https://github.com/github/cmark-gfm/archive/refs/tags/0.29.0.gfm.13.tar.gz)
FetchContent_MakeAvailable(cmark_gfm)
set_target_properties(cmark-gfm PROPERTIES EXCLUDE_FROM_ALL TRUE)
add_library(sos-ftxui-markdown STATIC ${SOS_FTXUI_MARKDOWN_SOURCES})
target_include_directories(sos-ftxui-markdown PRIVATE
    "${cmark_gfm_SOURCE_DIR}/src" "${cmark_gfm_BINARY_DIR}/src"
    "${cmark_gfm_SOURCE_DIR}/extensions" "${cmark_gfm_BINARY_DIR}/extensions")
target_compile_definitions(sos-ftxui-markdown PRIVATE CMARK_GFM_STATIC_DEFINE CMARK_GFM_EXTENSIONS_STATIC_DEFINE)
target_link_libraries(sos-ftxui-markdown PUBLIC ftxui::dom PRIVATE libcmark-gfm-extensions_static libcmark-gfm_static)
install(FILES "${cmark_gfm_SOURCE_DIR}/COPYING" DESTINATION share/sos-ftxui/licenses RENAME cmark-gfm.txt COMPONENT ftxui)
