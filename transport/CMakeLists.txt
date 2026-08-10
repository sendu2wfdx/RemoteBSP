if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "SocketCAN 传输层只能在 Linux 上构建")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(remotebsp_transport
    src/link_transport.cpp
    src/can_frame_codec.cpp
    src/usb_frame_codec.cpp
    src/mock_usb_transport.cpp
    src/libusb_transport.cpp
    src/socketcan_transport.cpp
)

target_include_directories(remotebsp_transport
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(remotebsp_transport PUBLIC PkgConfig::LIBUSB)

target_compile_options(remotebsp_transport PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wpedantic -Werror>
)
