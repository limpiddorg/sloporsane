find_program(UV_EXECUTABLE NAMES uv REQUIRED)

set(SOS_VERSION "dev" CACHE STRING "SOS release version, or dev to disable update checks")
if(NOT SOS_VERSION MATCHES "^(dev|v?[0-9]+\\.[0-9]+\\.[0-9]+(-[A-Za-z0-9.-]+)?)$")
    message(FATAL_ERROR "SOS_VERSION must be dev or a release version such as v0.0.1")
endif()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/VERSION" CONTENT "@SOS_VERSION@\n" @ONLY)

set(TUI_SOURCE_DIR "${PROJECT_SOURCE_DIR}/integrations/tui")
set(TUI_DIST_DIR "${PROJECT_BINARY_DIR}/tui/dist")
set(TUI_BUNDLE_DIR "${TUI_SOURCE_DIR}/src/sos_tui/bin")
set(TUI_ARCHIVE_NAME "sos-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}.tar.gz")
set(TUI_ARCHIVE "${TUI_DIST_DIR}/${TUI_ARCHIVE_NAME}")

file(GLOB_RECURSE TUI_SOURCES CONFIGURE_DEPENDS "${TUI_SOURCE_DIR}/src/sos_tui/*.py")
add_custom_command(
    OUTPUT "${TUI_ARCHIVE}" "${TUI_DIST_DIR}/sos/sos"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${TUI_BUNDLE_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:sosd>" "${TUI_BUNDLE_DIR}/sosd.new"
    COMMAND "${CMAKE_COMMAND}" -E rename "${TUI_BUNDLE_DIR}/sosd.new" "${TUI_BUNDLE_DIR}/sosd"
    COMMAND "${UV_EXECUTABLE}" sync --project "${TUI_SOURCE_DIR}" --group build --locked
    COMMAND "${CMAKE_COMMAND}" -E env
        "SOSD_BINARY=$<TARGET_FILE:sosd>" "SOS_TUI_SOURCE=${TUI_SOURCE_DIR}"
        "SOS_VERSION_FILE=${CMAKE_CURRENT_BINARY_DIR}/VERSION"
        "${UV_EXECUTABLE}" run --project "${TUI_SOURCE_DIR}" --no-sync python -m PyInstaller
        --noconfirm --log-level WARN --distpath "${TUI_DIST_DIR}" --workpath "${PROJECT_BINARY_DIR}/tui/pyinstaller"
        "${CMAKE_CURRENT_SOURCE_DIR}/sos.spec"
    COMMAND "${CMAKE_COMMAND}" -E chdir "${TUI_DIST_DIR}"
        "${CMAKE_COMMAND}" -E tar cfz "${TUI_ARCHIVE}" --format=gnutar sos
    DEPENDS sosd "${CMAKE_CURRENT_BINARY_DIR}/VERSION" ${TUI_SOURCES} "${TUI_SOURCE_DIR}/pyproject.toml" "${TUI_SOURCE_DIR}/uv.lock"
        "${CMAKE_CURRENT_SOURCE_DIR}/sos.spec" "${CMAKE_CURRENT_SOURCE_DIR}/entrypoint.py" "${PROJECT_SOURCE_DIR}/LICENSE"
    COMMENT "Building the standalone TUI with its bundled daemon"
    USES_TERMINAL
    VERBATIM
)
add_custom_target(build-tui DEPENDS "${TUI_ARCHIVE}" "${TUI_DIST_DIR}/sos/sos")

set(SOS_TUI_BIN_DIR "" CACHE PATH "Directory for the installed sos launcher")
if(NOT SOS_TUI_BIN_DIR)
    set(SOS_TUI_BIN_DIR "$ENV{HOME}/.local/bin")
endif()
if(IS_ABSOLUTE "$ENV{XDG_DATA_HOME}")
    set(TUI_DEFAULT_INSTALL_DIR "$ENV{XDG_DATA_HOME}/sos/app")
else()
    set(TUI_DEFAULT_INSTALL_DIR "$ENV{HOME}/.local/share/sos/app")
endif()
set(SOS_TUI_INSTALL_DIR "${TUI_DEFAULT_INSTALL_DIR}" CACHE PATH "Directory for standalone sos distributions")
configure_file(install.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/install-tui.cmake" @ONLY)
add_custom_target(install-tui ALL
    COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/install-tui.cmake"
    DEPENDS build-tui
    COMMENT "Installing the standalone sos distribution"
    VERBATIM
)

if(BUILD_TESTING)
    add_test(NAME TuiBundleTest.StandaloneStartup
        COMMAND "${UV_EXECUTABLE}" run --project "${TUI_SOURCE_DIR}" --no-sync python
            "${CMAKE_CURRENT_SOURCE_DIR}/smoke_test.py" "${TUI_ARCHIVE}")
    set_tests_properties(TuiBundleTest.StandaloneStartup PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 45)
endif()
