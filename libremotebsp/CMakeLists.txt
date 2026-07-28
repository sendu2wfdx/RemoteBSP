add_library(remotebsp
    src/client.cpp
)

target_include_directories(remotebsp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(remotebsp
    PUBLIC
        remotebsp_protocol
    PRIVATE
        remotebsp_request_manager
)

target_compile_options(remotebsp PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
