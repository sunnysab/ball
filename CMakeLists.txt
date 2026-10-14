cmake_minimum_required(VERSION 3.25)
project(ball VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(LayerShellQt REQUIRED)

add_library(ball_daemon STATIC
    src/daemon/interfaces.cpp
    src/daemon/traffic.cpp
)
target_include_directories(ball_daemon PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(ball_daemon PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ball_daemon PUBLIC Qt6::Core)

add_executable(ball src/main.cpp)
target_compile_definitions(ball PRIVATE BALL_VERSION="${PROJECT_VERSION}")
target_compile_options(ball PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ball PRIVATE Qt6::Widgets Qt6::Network LayerShellQt::Interface ball_daemon)

install(TARGETS ball RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 REQUIRED COMPONENTS Test)

    qt_standard_project_setup()

    qt_add_executable(ball_traffic_test tests/traffic.cpp)
    target_link_libraries(ball_traffic_test PRIVATE Qt6::Test ball_daemon)

    add_test(NAME ball_version COMMAND ball --version)
    set_tests_properties(ball_version PROPERTIES PASS_REGULAR_EXPRESSION "ball ${PROJECT_VERSION}")

    add_test(NAME ball_traffic COMMAND ball_traffic_test)
    # The core is GUI-free; keep the test runnable without a desktop session.
    set_tests_properties(ball_traffic PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
