add_library(remotebsp_device_params
    src/boot_epoch.c
    src/schema.c
    src/store.c
)

target_include_directories(remotebsp_device_params
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_SOURCE_DIR}/protocol/include
)

target_compile_features(remotebsp_device_params PUBLIC c_std_11)
target_compile_options(remotebsp_device_params PRIVATE
    $<$<C_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
    $<$<C_COMPILER_ID:MSVC>:/W4 /WX>
)
