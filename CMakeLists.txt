cmake_minimum_required(VERSION 3.25)
project(ball VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(LayerShellQt REQUIRED)
include(GNUInstallDirs)

set(BALL_SOCKTRAIL_BINARY "" CACHE FILEPATH "External socktrail executable to install")

# Before any target: the daemon library has QObject sources, and AUTOMOC is
# enabled per target at creation time.
qt_standard_project_setup()

add_library(ball_process STATIC src/process.cpp)
target_include_directories(ball_process PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ball_process PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ball_process PUBLIC Qt6::Core)

add_library(ball_daemon STATIC
    src/daemon/interfaces.cpp
    src/daemon/traffic.cpp
    src/daemon/processes.cpp
    src/daemon/socktrail.cpp
    src/daemon/service.cpp
)
target_include_directories(ball_daemon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ball_daemon PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ball_daemon PUBLIC Qt6::Core Qt6::Network ball_process)

add_library(ball_ui STATIC
    src/ui/snapshot.cpp
    src/ui/snapshot_client.cpp
    src/ui/format.cpp
    src/ui/traffic_model.cpp
    src/ui/placement.cpp
    src/ui/preferences.cpp
    src/ui/autostart.cpp
    src/ui/appearance.cpp
    src/ui/details.cpp
    src/ui/hud.cpp
)
target_include_directories(ball_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ball_ui PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ball_ui PUBLIC Qt6::Widgets Qt6::Network LayerShellQt::Interface ball_process)

add_executable(ball src/main.cpp)
target_compile_definitions(ball PRIVATE
    BALL_VERSION="${PROJECT_VERSION}"
    BALL_DEFAULT_COLLECTOR_PATH="${CMAKE_INSTALL_FULL_LIBEXECDIR}/ball/socktrail"
)
target_compile_options(ball PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ball PRIVATE ball_ui ball_daemon)

install(TARGETS ball RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
configure_file(packaging/ball@.service.in ball@.service @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ball@.service DESTINATION lib/systemd/system)
configure_file(packaging/ball.desktop.in ball.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ball.desktop
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/applications)
install(FILES packaging/ball.svg
    DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/icons/hicolor/scalable/apps)

if(BALL_SOCKTRAIL_BINARY)
    if(NOT EXISTS "${BALL_SOCKTRAIL_BINARY}" OR IS_DIRECTORY "${BALL_SOCKTRAIL_BINARY}")
        message(FATAL_ERROR "BALL_SOCKTRAIL_BINARY must name an existing executable file")
    endif()
    install(PROGRAMS "${BALL_SOCKTRAIL_BINARY}"
        DESTINATION ${CMAKE_INSTALL_LIBEXECDIR}/ball RENAME socktrail)
else()
    message(WARNING "No collector will be installed; provide BALL_SOCKTRAIL_BINARY or --socktrail-bin")
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_add_executable(ball_traffic_test tests/traffic.cpp)
    target_link_libraries(ball_traffic_test PRIVATE Qt6::Test ball_daemon)

    qt_add_executable(ball_process_test tests/process.cpp)
    target_link_libraries(ball_process_test PRIVATE Qt6::Test ball_daemon)

    qt_add_executable(ball_processes_test tests/processes.cpp)
    target_link_libraries(ball_processes_test PRIVATE Qt6::Test ball_daemon)

    qt_add_executable(ball_socktrail_test tests/socktrail.cpp)
    target_link_libraries(ball_socktrail_test PRIVATE Qt6::Test ball_daemon)

    qt_add_executable(ball_service_test tests/service.cpp)
    target_link_libraries(ball_service_test PRIVATE Qt6::Test ball_daemon)
    # The fake collector is a separate process, exactly like the real external
    # socktrail binary; the test only needs its path.
    add_executable(ball_fake_collector tests/fake_collector.cpp)
    target_link_libraries(ball_fake_collector PRIVATE Qt6::Core Qt6::Network)
    target_compile_options(ball_fake_collector PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_definitions(ball_service_test PRIVATE
        BALL_FAKE_COLLECTOR_BINARY="$<TARGET_FILE:ball_fake_collector>")
    add_dependencies(ball_service_test ball_fake_collector)

    qt_add_executable(ball_ui_test tests/ui.cpp)
    target_link_libraries(ball_ui_test PRIVATE Qt6::Test ball_ui)

    qt_add_executable(ball_autostart_test tests/autostart.cpp)
    # GUI-free: the helper only touches files, so the test needs no display.
    target_link_libraries(ball_autostart_test PRIVATE Qt6::Test Qt6::Core ball_ui)

    qt_add_executable(ball_ui_preview tests/ui_preview.cpp)
    target_link_libraries(ball_ui_preview PRIVATE ball_ui)

    qt_add_executable(ball_cli_test tests/cli.cpp)
    target_link_libraries(ball_cli_test PRIVATE Qt6::Test Qt6::Network)
    target_compile_definitions(ball_cli_test PRIVATE
        BALL_EXECUTABLE="$<TARGET_FILE:ball>"
        BALL_FAKE_COLLECTOR_BINARY="$<TARGET_FILE:ball_fake_collector>")
    add_dependencies(ball_cli_test ball ball_fake_collector)

    add_test(NAME ball_ui COMMAND ball_ui_test)
    set_tests_properties(ball_ui PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 90)

    add_test(NAME ball_autostart COMMAND ball_autostart_test)
    set_tests_properties(ball_autostart PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 90)

    add_test(NAME ball_version COMMAND ball --version)
    set_tests_properties(ball_version PROPERTIES PASS_REGULAR_EXPRESSION "ball ${PROJECT_VERSION}")

    add_test(NAME ball_traffic COMMAND ball_traffic_test)
    # The core is GUI-free; keep the test runnable without a desktop session.
    set_tests_properties(ball_traffic PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME ball_process COMMAND ball_process_test)
    set_tests_properties(ball_process PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME ball_processes COMMAND ball_processes_test)
    set_tests_properties(ball_processes PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME ball_socktrail COMMAND ball_socktrail_test)
    set_tests_properties(ball_socktrail PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

    add_test(NAME ball_service COMMAND ball_service_test)
    set_tests_properties(ball_service PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120)

    # Drives the real executable over its command line, with no display and no
    # Wayland session in the environment.
    add_test(NAME ball_cli COMMAND ball_cli_test)
    set_tests_properties(ball_cli PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
        TIMEOUT 120)
endif()
