cmake_minimum_required(VERSION 3.16)
project(remote_bsp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(protocol)
add_subdirectory(transport)
add_subdirectory(mock_mcu)
add_subdirectory(toolbusd)
add_subdirectory(libremotebsp)

add_executable(remote-cli toolbusd/src/remote_cli.cpp)
target_link_libraries(remote-cli PRIVATE remotebsp)
target_compile_options(remote-cli PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)

include(CTest)
if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
