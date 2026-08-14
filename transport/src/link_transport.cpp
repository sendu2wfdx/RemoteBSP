#include "remotebsp/transport/link_transport.hpp"

namespace remotebsp::transport {

bool is_can_link(LinkKind kind) noexcept {
    return kind == LinkKind::ClassicalCan || kind == LinkKind::CanFd;
}

const char* link_kind_name(LinkKind kind) noexcept {
    switch (kind) {
        case LinkKind::ClassicalCan:
            return "classical-can";
        case LinkKind::CanFd:
            return "can-fd";
        case LinkKind::Usb:
            return "usb";
        case LinkKind::MockUsb:
            return "mock-usb";
    }
    return "unknown";
}

}
