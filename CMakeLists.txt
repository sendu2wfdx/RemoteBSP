cmake_minimum_required(VERSION 3.16)
project(remote_bsp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 干净构建必须在任何子目录注册 Python 测试前解析解释器；不能依赖旧缓存
# 偶然留下 Python3_EXECUTABLE。
include(CTest)
if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
endif()

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

if(BUILD_TESTING)
    add_subdirectory(tests)
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
    add_test(NAME comparison_plan_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest
                tests.comparison_plan_tests
                tests.benchmark_run_manifest_tests -v)
    set_tests_properties(comparison_plan_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME generated_pin_choices_are_current
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/firmware/scripts/generate_pin_choices.py --check)
    add_test(NAME deployment_evidence_offline_tests
        COMMAND ${Python3_EXECUTABLE} -m unittest
                gui.tests.test_deployment_evidence_bundle -v)
    set_tests_properties(deployment_evidence_offline_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    add_test(NAME clean_build_registration_tests
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/clean_build_registration_tests.py
                ${CMAKE_COMMAND} ${CMAKE_CTEST_COMMAND}
                ${CMAKE_SOURCE_DIR})
    set_tests_properties(clean_build_registration_tests PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        TIMEOUT 60)
endif()
