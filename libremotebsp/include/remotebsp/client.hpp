#pragma once

#include "remotebsp/protocol/packet.hpp"
#include "remotebsp/protocol/resource.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <optional>
#include <string>
#include <vector>

namespace remotebsp {

struct NodeInfo {
    std::array<std::uint8_t, 16> uuid{};
    std::uint16_t firmware_major{};
    std::uint16_t firmware_minor{};
    std::uint16_t firmware_patch{};
    std::uint32_t board_type{};
    std::uint8_t protocol_version{};
};

struct DiscoveredNode {
    NodeInfo identity;
    std::uint32_t node_id{};
    bool online{};
    bool ready{};
};

enum class GpioDirection : std::uint8_t {
    Input = 0,
    Output = 1,
};

enum class UartParity : std::uint8_t {
    None = 0,
    Odd = 1,
    Even = 2,
};

struct UartConfig {
    std::uint8_t port{};
    std::uint32_t baud_rate{};
    std::uint8_t data_bits{8};
    std::uint8_t stop_bits{1};
    UartParity parity{UartParity::None};
};

class ClientException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RemoteException : public ClientException {
public:
    RemoteException(std::uint8_t status, const std::string& message);
    std::uint8_t status() const noexcept;

private:
    std::uint8_t status_;
};

// 同步客户端。每次调用使用独立 Unix Domain Socket 连接，因此同一实例可以
// 被多个线程并发调用，且一个调用超时不会污染其他调用的响应流。
class Client {
public:
    explicit Client(std::string socket_path = "/tmp/toolbusd.sock",
                    std::uint32_t node_id = 1);

    std::vector<std::uint8_t> ping(
        const std::vector<std::uint8_t>& data) const;
    NodeInfo get_info() const;
    std::uint64_t get_capabilities() const;
    std::vector<DiscoveredNode> list_nodes() const;
    std::optional<protocol::Packet> next_event() const;
    void enter_bootloader() const;
    void enter_usb_bootloader() const;

    std::vector<protocol::ResourceDescriptor> list_resources() const;
    protocol::ResourceDescriptor describe_resource(
        std::uint32_t resource_id) const;
    protocol::ResourceStatusPayload resource_status(
        std::uint32_t resource_id) const;
    void reset_resource(std::uint32_t resource_id) const;

    std::uint32_t gpio_create(std::uint16_t pin,
                              GpioDirection direction,
                              bool initial_value = false) const;
    bool gpio_read(std::uint32_t object_id) const;
    void gpio_write(std::uint32_t object_id, bool value) const;

    std::uint32_t uart_create(const UartConfig& config) const;
    std::vector<std::uint8_t> uart_read(
        std::uint32_t object_id, std::size_t maximum_length) const;
    void uart_write(std::uint32_t object_id,
                    const std::vector<std::uint8_t>& data) const;

    protocol::Packet transact(protocol::Packet request) const;
    const std::string& socket_path() const noexcept;
    std::uint32_t node_id() const noexcept;

private:
    protocol::Packet command(protocol::Command command,
                             std::vector<std::uint8_t> payload = {},
                             std::uint32_t object_id = 0) const;

    std::string socket_path_;
    std::uint32_t node_id_;
};

}
