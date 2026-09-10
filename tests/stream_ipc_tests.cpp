#include "remotebsp/toolbusd/ipc.hpp"

#include <cassert>
#include <sys/socket.h>
#include <unistd.h>

int main() {
    int sockets[2]{};
    assert(::socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    remotebsp::toolbusd::write_ipc_stream_read_request(
        sockets[0], 7U, 42U, 9U, 1234U);
    const auto request = remotebsp::toolbusd::read_ipc_request(sockets[1]);
    assert(request.kind == remotebsp::toolbusd::IpcRequestKind::StreamRead);
    assert(request.node_id == 7U);
    assert(request.stream_id == 42U);
    assert(request.expected_sequence == 9U);
    assert(request.timeout_ms == 1234U);
    ::close(sockets[0]);
    ::close(sockets[1]);
}
