cmake_minimum_required(VERSION 3.16)
project(remote_bsp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_subdirectory(device_params)
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
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME gui_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s ${CMAKE_SOURCE_DIR}/gui/tests -v)
    add_test(NAME runtime_api_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest discover
                -s ${CMAKE_SOURCE_DIR}/runtime_api/tests -v)
    set_tests_properties(runtime_api_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME maturity_manifest_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/maturity_manifest_tests.py)
    set_tests_properties(maturity_manifest_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME generated_pin_choices_are_current
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/firmware/scripts/generate_pin_choices.py --check)
endif()
