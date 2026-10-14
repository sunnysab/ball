cmake_minimum_required(VERSION 3.25)
project(ball VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
find_package(LayerShellQt REQUIRED)

add_executable(ball src/main.cpp)
target_compile_definitions(ball PRIVATE BALL_VERSION="${PROJECT_VERSION}")
target_compile_options(ball PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(ball PRIVATE Qt6::Widgets Qt6::Network LayerShellQt::Interface)

install(TARGETS ball RUNTIME DESTINATION bin)

include(CTest)
if(BUILD_TESTING)
    add_test(NAME ball_version COMMAND ball --version)
    set_tests_properties(ball_version PROPERTIES PASS_REGULAR_EXPRESSION "ball ${PROJECT_VERSION}")
endif()
