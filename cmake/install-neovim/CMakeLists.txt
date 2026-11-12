option(SOS_INSTALL_NEOVIM "Install the native plugin when Neovim 0.10+ is available" ON)
if(NOT SOS_INSTALL_NEOVIM)
    return()
endif()
find_program(SOS_NEOVIM_EXECUTABLE NAMES nvim)
if(NOT SOS_NEOVIM_EXECUTABLE)
    message(STATUS "Neovim not found; skipping SOS plugin installation")
    return()
endif()
# Query Neovim itself to respect XDG_DATA_HOME and NVIM_APPNAME, without user configuration.
execute_process(
    COMMAND "${SOS_NEOVIM_EXECUTABLE}" --headless -u NONE -i NONE -n
        -c "lua if vim.fn.has('nvim-0.10') == 0 then vim.cmd('cquit') else io.write(vim.fn.stdpath('data')) end"
        -c "qa!"
    RESULT_VARIABLE NVIM_RESULT OUTPUT_VARIABLE NVIM_DATA ERROR_VARIABLE NVIM_ERROR
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT NVIM_RESULT EQUAL 0 OR NOT NVIM_DATA)
    message(STATUS "Neovim 0.10+ required; skipping SOS plugin installation: ${NVIM_ERROR}")
    return()
endif()
set(SOS_NEOVIM_INSTALL_DIR "${NVIM_DATA}/site/pack/sos/start/sos" CACHE PATH "Native SOS Neovim plugin destination")
add_custom_target(install-neovim ALL
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SOS_NEOVIM_INSTALL_DIR}"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/integrations/neovim/lua" "${SOS_NEOVIM_INSTALL_DIR}/lua"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/integrations/neovim/plugin" "${SOS_NEOVIM_INSTALL_DIR}/plugin"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory
        "${PROJECT_SOURCE_DIR}/integrations/neovim/scripts" "${SOS_NEOVIM_INSTALL_DIR}/scripts"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${SOS_NEOVIM_INSTALL_DIR}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:sosd>" "${SOS_NEOVIM_INSTALL_DIR}/bin/sosd.new"
    COMMAND "${CMAKE_COMMAND}" -E rename "${SOS_NEOVIM_INSTALL_DIR}/bin/sosd.new" "${SOS_NEOVIM_INSTALL_DIR}/bin/sosd"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/integrations/neovim/README.md" "${SOS_NEOVIM_INSTALL_DIR}/README.md"
    DEPENDS sosd
    COMMENT "Installing the native SOS Neovim plugin and daemon"
    VERBATIM)
message(STATUS "SOS Neovim plugin destination: ${SOS_NEOVIM_INSTALL_DIR}")
