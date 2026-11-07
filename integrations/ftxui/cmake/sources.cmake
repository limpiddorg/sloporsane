set(SOS_FTXUI_CORE_SOURCES
    client.hpp client.cpp
    model.hpp model.cpp
    attachments.hpp attachments.cpp
    clipboard.cpp
    draft_buffer.hpp draft_buffer.cpp
    history_viewport.hpp history_viewport.cpp
)
set(SOS_FTXUI_SOURCES
    main.cpp
    app.hpp app.cpp
    composer.hpp composer.cpp
    tool_view.hpp tool_view.cpp
    status_bar.hpp status_bar.cpp
)
set(SOS_FTXUI_MARKDOWN_SOURCES markdown.hpp markdown.cpp)
set(SOS_FTXUI_TEST_SOURCES tests/core_test.cpp tests/composer_test.cpp composer.hpp composer.cpp status_bar.hpp status_bar.cpp tool_view.hpp tool_view.cpp)
set(SOS_FTXUI_E2E_TEST_SOURCES tests/e2e_test.cpp)
set(SOS_FTXUI_FIXTURE_SOURCES tests/fixture_daemon.cpp)
