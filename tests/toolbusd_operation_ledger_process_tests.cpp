#include "remotebsp/client.hpp"
#include "remotebsp/toolbusd/ipc.hpp"
#include "remotebsp/toolbusd/operation_ledger.hpp"
#include "remotebsp/toolbusd/runtime_control.hpp"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <future>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                     \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << " 检查失败: " #condition << '\n';                    \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

template <std::size_t Size>
std::array<std::uint8_t, Size> filled(std::uint8_t value) {
    std::array<std::uint8_t, Size> result{};
    result.fill(value);
    return result;
}

class TestDirectory {
public:
    TestDirectory() {
        std::string pattern = "/tmp/remotebsp-ledger-process-XXXXXX";
        pattern.push_back('\0');
        auto* created = ::mkdtemp(pattern.data());
        if (created == nullptr) {
            throw std::runtime_error("创建进程测试目录失败");
        }
        path_ = created;
    }

    ~TestDirectory() { std::filesystem::remove_all(path_); }

    std::filesystem::path child(const std::string& name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_;
};

class ChildProcess {
public:
    ChildProcess() = default;
    explicit ChildProcess(pid_t pid) : pid_(pid) {}
    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept : pid_(other.pid_) {
        other.pid_ = -1;
    }
    ChildProcess& operator=(ChildProcess&& other) noexcept {
        stop();
        pid_ = other.pid_;
        other.pid_ = -1;
        return *this;
    }
    ~ChildProcess() { stop(); }

    void send_signal(int signal) const {
        if (pid_ <= 0 || ::kill(pid_, signal) != 0) {
            throw std::runtime_error("向子进程发送信号失败");
        }
    }

    void stop() noexcept {
        if (pid_ <= 0) return;
        static_cast<void>(::kill(pid_, SIGTERM));
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
        }
        pid_ = -1;
    }

private:
    pid_t pid_{-1};
};

pid_t spawn(const std::vector<std::string>& arguments) {
    const auto pid = ::fork();
    if (pid < 0) throw std::runtime_error("fork失败");
    if (pid == 0) {
        std::vector<char*> argv;
        argv.reserve(arguments.size() + 1U);
        for (const auto& argument : arguments) {
            argv.push_back(const_cast<char*>(argument.c_str()));
        }
        argv.push_back(nullptr);
        ::execv(argv.front(), argv.data());
        ::_exit(127);
    }
    return pid;
}

bool wait_for_path(const std::filesystem::path& path,
                   std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        struct stat status {};
        if (::lstat(path.c_str(), &status) == 0 && S_ISSOCK(status.st_mode)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

remotebsp::DiscoveredNode wait_for_ready_node(
    const std::string& ipc, std::chrono::seconds timeout) {
    remotebsp::Client client(ipc, 1U);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            const auto nodes = client.list_nodes();
            if (!nodes.empty() && nodes.front().online && nodes.front().ready) {
                return nodes.front();
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    throw std::runtime_error("未发现可用 Mock MCU");
}

template <typename Call>
void expect_ipc_error(std::uint16_t code, Call&& call) {
    try {
        call();
    } catch (const remotebsp::IpcErrorException& error) {
        CHECK(error.code() == code);
        CHECK(!error.possibly_committed());
        return;
    }
    CHECK(false);
}

ChildProcess start_daemon(const std::string& executable,
                          const TestDirectory& directory,
                          const std::string& ledger_name = "ledger",
                          std::vector<std::string> extra_options = {}) {
    const auto usb = directory.child("usb.sock").string();
    const auto ipc = directory.child("toolbusd.sock").string();
    const auto ledger = directory.child(ledger_name).string();
    std::vector<std::string> arguments{
        executable, usb, "usb-mock", ipc,
        "--runtime-operation-ledger-dir", ledger};
    arguments.insert(arguments.end(), extra_options.begin(),
                     extra_options.end());
    ChildProcess process(spawn(arguments));
    CHECK(wait_for_path(ipc, std::chrono::seconds(5)));
    return process;
}

void check_required_option(const std::string& toolbusd) {
    TestDirectory directory;
    const auto pid = spawn({toolbusd, directory.child("usb.sock").string(),
                            "usb-mock",
                            directory.child("toolbusd.sock").string()});
    int status = 0;
    CHECK(::waitpid(pid, &status, 0) == pid);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) != 0);
    CHECK(!std::filesystem::exists(directory.child("toolbusd.sock")));
}

void check_live_routing(const std::string& toolbusd,
                        const std::string& mock_mcu) {
    std::cerr << "阶段: live-routing 启动\n";
    TestDirectory directory;
    const auto usb = directory.child("usb.sock").string();
    const auto ipc = directory.child("toolbusd.sock").string();
    auto daemon = start_daemon(toolbusd, directory);
    CHECK(wait_for_path(usb, std::chrono::seconds(5)));
    ChildProcess node(spawn({mock_mcu, usb, "usb-mock"}));

    remotebsp::Client client(ipc, 1U);
    bool discovered = false;
    std::optional<remotebsp::DiscoveredNode> discovered_node;
    const auto discovery_deadline = std::chrono::steady_clock::now() +
                                    std::chrono::seconds(7);
    while (std::chrono::steady_clock::now() < discovery_deadline) {
        try {
            const auto nodes = client.list_nodes();
            discovered = !nodes.empty() && nodes.front().online &&
                         nodes.front().ready;
            if (discovered) {
                discovered_node = nodes.front();
                break;
            }
        } catch (const std::exception&) {
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(discovered);
    if (!discovered_node.has_value()) {
        throw std::runtime_error("未发现可用 Mock MCU");
    }
    std::cerr << "阶段: 节点已发现\n";

    remotebsp::Client node_client(ipc, discovered_node->node_id);
    const auto daemon_id = node_client.daemon_identity().instance_id;
    const auto lease = filled<16>(0x31U);
    const auto node_uuid = discovered_node->identity.uuid;
    constexpr std::uint32_t resource_id = 0x01000000U;
    node_client.runtime_control_acquire(
        daemon_id, lease, node_uuid, "owner", resource_id, 30000U);
    std::cerr << "阶段: acquire 完成\n";

    expect_ipc_error(
        static_cast<std::uint16_t>(
            remotebsp::toolbusd::IpcErrorCode::UnsupportedRequest),
        [&] {
            static_cast<void>(node_client.runtime_gpio_write(
                daemon_id, lease, node_uuid, "owner", resource_id,
                "legacy", true));
        });

    const auto written = node_client.runtime_gpio_write_operation(
        daemon_id, lease, node_uuid, "owner", resource_id, "write-1", true);
    CHECK(written.kind == remotebsp::RuntimeOperationKind::GpioWrite);
    CHECK(written.state == remotebsp::RuntimeOperationState::Committed);
    CHECK(!written.replayed);
    CHECK(written.object_id.has_value());
    CHECK(written.value == true);
    std::cerr << "阶段: write operation 完成\n";

    const auto status = node_client.runtime_operation_status(
        daemon_id, "owner", written.operation_id);
    CHECK(status.replayed);
    CHECK(status.state == remotebsp::RuntimeOperationState::Committed);
    const auto located = node_client.runtime_operation_lookup(
        daemon_id, "owner", remotebsp::RuntimeOperationKind::GpioWrite,
        lease, "write-1");
    CHECK(located.operation_id == written.operation_id);
    CHECK(located.replayed);

    auto wrong_daemon = daemon_id;
    wrong_daemon[0] ^= 0xffU;
    expect_ipc_error(
        static_cast<std::uint16_t>(
            remotebsp::toolbusd::IpcErrorCode::DaemonIdentityMismatch),
        [&] {
            static_cast<void>(node_client.runtime_operation_status(
                wrong_daemon, "owner", written.operation_id));
        });
    const auto private_status = node_client.runtime_operation_status(
        daemon_id, "other-owner", written.operation_id);
    CHECK(private_status.kind == remotebsp::RuntimeOperationKind::Unknown);
    CHECK(private_status.state ==
          remotebsp::RuntimeOperationState::ExpiredUnknown);
    const auto private_miss = node_client.runtime_operation_lookup(
        daemon_id, "other-owner",
        remotebsp::RuntimeOperationKind::GpioWrite, lease, "write-1");
    CHECK(private_miss.kind == remotebsp::RuntimeOperationKind::GpioWrite);
    CHECK(private_miss.state ==
          remotebsp::RuntimeOperationState::ExpiredUnknown);

    expect_ipc_error(
        static_cast<std::uint16_t>(
            remotebsp::toolbusd::IpcErrorCode::UnsupportedRequest),
        [&] { node_client.runtime_control_release(daemon_id, lease, "owner"); });
    const auto released = node_client.runtime_control_release_operation(
        daemon_id, lease, "owner");
    std::cerr << "阶段: release operation 完成\n";
    CHECK(released.kind == remotebsp::RuntimeOperationKind::ControlRelease);
    CHECK(released.state == remotebsp::RuntimeOperationState::Committed);
    CHECK(released.recovery ==
          remotebsp::RuntimeOperationRecovery::SafeClosed);
    CHECK(!released.replayed);
    const auto release_replay = node_client.runtime_control_release_operation(
        daemon_id, lease, "owner");
    CHECK(release_replay.replayed);
    CHECK(release_replay.operation_id == released.operation_id);

    // 同一 daemon 内即使调用者以完全相同 scope 重用 lease ID，新登记也
    // 拥有独立生命周期；旧 Release 终态不得冒充本次释放成功。
    node_client.runtime_control_acquire(
        daemon_id, lease, node_uuid, "owner", resource_id, 30000U);
    expect_ipc_error(
        static_cast<std::uint16_t>(
            remotebsp::toolbusd::IpcErrorCode::IdempotencyConflict),
        [&] {
            static_cast<void>(
                node_client.runtime_control_release_operation(
                    daemon_id, lease, "owner"));
        });
    const auto active_after_conflict =
        node_client.runtime_gpio_write_operation(
            daemon_id, lease, node_uuid, "owner", resource_id,
            "new-lifecycle-write", false);
    CHECK(active_after_conflict.state ==
          remotebsp::RuntimeOperationState::Committed);
    CHECK(!active_after_conflict.replayed);

    std::cerr << "阶段: live-routing 结束\n";
}

void check_concurrent_gpio_replay(const std::string& toolbusd,
                                  const std::string& mock_mcu) {
    std::cerr << "阶段: concurrent-replay 启动\n";
    TestDirectory directory;
    const auto usb = directory.child("usb.sock").string();
    const auto ipc = directory.child("toolbusd.sock").string();
    auto daemon = start_daemon(
        toolbusd, directory, "ledger",
        {"--test-runtime-gpio-post-lookup-barrier", "2"});
    CHECK(wait_for_path(usb, std::chrono::seconds(5)));
    ChildProcess node(spawn({mock_mcu, usb, "usb-mock"}));
    const auto discovered = wait_for_ready_node(ipc, std::chrono::seconds(7));
    remotebsp::Client client(ipc, discovered.node_id);
    const auto daemon_id = client.daemon_identity().instance_id;
    const auto lease = filled<16>(0x41U);
    const auto node_uuid = discovered.identity.uuid;
    constexpr std::uint32_t resource_id = 0x01000000U;
    client.runtime_control_acquire(
        daemon_id, lease, node_uuid, "owner", resource_id, 30000U);

    // 测试构建专用屏障位于 main 的初始 Absent lookup 之后。两个请求均
    // 到达后才一起进入 Gate，因此后到者只能命中 Gate 内存 replay，并
    // 必须由 main 二次读取已提交账本才能得到成功响应。
    remotebsp::Client first_client(ipc, discovered.node_id);
    remotebsp::Client second_client(ipc, discovered.node_id);
    auto first = std::async(std::launch::async, [&] {
        return first_client.runtime_gpio_write_operation(
            daemon_id, lease, node_uuid, "owner", resource_id,
            "concurrent-target", true);
    });
    auto second = std::async(std::launch::async, [&] {
        return second_client.runtime_gpio_write_operation(
            daemon_id, lease, node_uuid, "owner", resource_id,
            "concurrent-target", true);
    });
    const auto first_result = first.get();
    const auto second_result = second.get();
    CHECK(first_result.state == remotebsp::RuntimeOperationState::Committed);
    CHECK(second_result.state == remotebsp::RuntimeOperationState::Committed);
    CHECK(first_result.operation_id == second_result.operation_id);
    CHECK(first_result.replayed != second_result.replayed);
    static_cast<void>(client.runtime_control_release_operation(
        daemon_id, lease, "owner"));
    std::cerr << "阶段: concurrent-replay 结束\n";
}

struct ObservedIpcError {
    std::uint16_t code{};
    bool retryable{};
    bool possibly_committed{};
    bool received{};
};

template <typename Call>
ObservedIpcError observe_ipc_error(Call&& call) {
    ObservedIpcError observed;
    try {
        call();
    } catch (const remotebsp::IpcErrorException& error) {
        observed.code = error.code();
        observed.retryable = error.retryable();
        observed.possibly_committed = error.possibly_committed();
        observed.received = true;
    }
    return observed;
}

void check_uncertain_terminal_error(const ObservedIpcError& observed) {
    CHECK(observed.received);
    CHECK(observed.code == static_cast<std::uint16_t>(
        remotebsp::toolbusd::IpcErrorCode::BackendUnavailable));
    CHECK(!observed.retryable);
    CHECK(observed.possibly_committed);
}

void check_gpio_terminal_persistence_failure_flags(
    const std::string& toolbusd, const std::string& mock_mcu) {
    std::cerr << "阶段: gpio-terminal-failure-flags 启动\n";
    TestDirectory directory;
    const auto usb = directory.child("usb.sock").string();
    const auto ipc = directory.child("toolbusd.sock").string();
    auto daemon = start_daemon(
        toolbusd, directory, "ledger",
        {"--test-operation-ledger-fail-terminal-sync", "1"});
    CHECK(wait_for_path(usb, std::chrono::seconds(5)));
    ChildProcess node(spawn({mock_mcu, usb, "usb-mock"}));
    const auto discovered = wait_for_ready_node(ipc, std::chrono::seconds(7));
    remotebsp::Client client(ipc, discovered.node_id);
    const auto daemon_id = client.daemon_identity().instance_id;
    const auto lease = filled<16>(0x51U);
    const auto node_uuid = discovered.identity.uuid;
    constexpr std::uint32_t resource_id = 0x01000000U;
    client.runtime_control_acquire(
        daemon_id, lease, node_uuid, "owner", resource_id, 30000U);

    const auto observed = observe_ipc_error([&] {
        static_cast<void>(client.runtime_gpio_write_operation(
            daemon_id, lease, node_uuid, "owner", resource_id,
            "terminal-failure", true));
    });
    check_uncertain_terminal_error(observed);
    std::cerr << "阶段: gpio-terminal-failure-flags 结束\n";
}

void check_release_terminal_persistence_failure_flags(
    const std::string& toolbusd, const std::string& mock_mcu) {
    std::cerr << "阶段: release-terminal-failure-flags 启动\n";
    TestDirectory directory;
    const auto usb = directory.child("usb.sock").string();
    const auto ipc = directory.child("toolbusd.sock").string();
    auto daemon = start_daemon(
        toolbusd, directory, "ledger",
        {"--test-operation-ledger-fail-terminal-sync", "2"});
    CHECK(wait_for_path(usb, std::chrono::seconds(5)));
    ChildProcess node(spawn({mock_mcu, usb, "usb-mock"}));
    const auto discovered = wait_for_ready_node(ipc, std::chrono::seconds(7));
    remotebsp::Client client(ipc, discovered.node_id);
    const auto daemon_id = client.daemon_identity().instance_id;
    const auto lease = filled<16>(0x52U);
    client.runtime_control_acquire(
        daemon_id, lease, discovered.identity.uuid, "owner",
        0x01000000U, 30000U);
    const auto written = client.runtime_gpio_write_operation(
        daemon_id, lease, discovered.identity.uuid, "owner",
        0x01000000U, "release-terminal-setup", true);
    CHECK(written.state == remotebsp::RuntimeOperationState::Committed);

    const auto observed = observe_ipc_error([&] {
        static_cast<void>(client.runtime_control_release_operation(
            daemon_id, lease, "owner"));
    });
    check_uncertain_terminal_error(observed);
    std::cerr << "阶段: release-terminal-failure-flags 结束\n";
}

void check_bus_reset_unknown_and_restart(
    const std::string& toolbusd, const std::string& mock_mcu,
    const std::string& bus_manifest) {
    std::cerr << "阶段: bus-reset-unknown-restart 启动\n";
    TestDirectory directory;
    const auto usb = directory.child("usb.sock").string();
    const auto ipc = directory.child("toolbusd.sock").string();
    auto daemon = start_daemon(toolbusd, directory, "ledger",
                               {"--test-bus-reset-drop-response", "1"});
    CHECK(wait_for_path(usb, std::chrono::seconds(5)));
    ChildProcess node(spawn(
        {mock_mcu, usb, "usb-mock", "--board", bus_manifest}));
    const auto discovered = wait_for_ready_node(ipc, std::chrono::seconds(7));
    remotebsp::Client client(ipc, discovered.node_id);
    const auto daemon_id = client.daemon_identity().instance_id;
    const auto node_uuid = discovered.identity.uuid;
    const auto target_lease = filled<16>(0x61U);
    const auto peer_lease = filled<16>(0x62U);
    constexpr std::uint32_t target_resource = 201326593U;
    constexpr std::uint32_t peer_resource = 234881025U;

    client.runtime_control_acquire(
        daemon_id, target_lease, node_uuid, "bus-owner", target_resource,
        30000U, remotebsp::toolbusd::kRuntimePermissionBusReset);
    const auto first = client.runtime_bus_resource_reset_operation(
        daemon_id, target_lease, node_uuid, "bus-owner", target_resource,
        "lost-response");
    CHECK(first.kind == remotebsp::RuntimeOperationKind::BusResourceReset);
    CHECK(first.state == remotebsp::RuntimeOperationState::Unknown);
    CHECK(first.recovery ==
          remotebsp::RuntimeOperationRecovery::ScopeBlocked);
    CHECK(!first.replayed);

    // 相同 selector 只能查询已持久化终态；若这里误发第二次，测试钩子
    // 的 ordinal 已经过期，结果会错误变成 Committed。
    const auto replay = client.runtime_bus_resource_reset_operation(
        daemon_id, target_lease, node_uuid, "bus-owner", target_resource,
        "lost-response");
    CHECK(replay.operation_id == first.operation_id);
    CHECK(replay.state == remotebsp::RuntimeOperationState::Unknown);
    CHECK(replay.recovery ==
          remotebsp::RuntimeOperationRecovery::ScopeBlocked);
    CHECK(replay.replayed);
    const auto status = client.runtime_operation_status(
        daemon_id, "bus-owner", first.operation_id);
    CHECK(status.state == remotebsp::RuntimeOperationState::Unknown);
    CHECK(status.replayed);

    expect_ipc_error(
        static_cast<std::uint16_t>(
            remotebsp::toolbusd::IpcErrorCode::IdempotencyConflict),
        [&] {
            static_cast<void>(client.runtime_bus_resource_reset_operation(
                daemon_id, target_lease, node_uuid, "bus-owner",
                peer_resource, "lost-response"));
        });
    expect_ipc_error(
        static_cast<std::uint16_t>(
            remotebsp::toolbusd::IpcErrorCode::LeaseConflict),
        [&] {
            client.runtime_control_acquire(
                daemon_id, filled<16>(0x63U), node_uuid, "bus-owner",
                target_resource, 30000U,
                remotebsp::toolbusd::kRuntimePermissionBusReset);
        });

    // 冻结键精确到节点资源；同节点的另一 SPI 设备仍可取得租约并复位。
    client.runtime_control_acquire(
        daemon_id, peer_lease, node_uuid, "bus-owner", peer_resource,
        30000U, remotebsp::toolbusd::kRuntimePermissionBusReset);
    const auto peer = client.runtime_bus_resource_reset_operation(
        daemon_id, peer_lease, node_uuid, "bus-owner", peer_resource,
        "peer-reset");
    CHECK(peer.state == remotebsp::RuntimeOperationState::Committed);
    CHECK(peer.recovery == remotebsp::RuntimeOperationRecovery::SafeClosed);

    daemon.stop();
    daemon = start_daemon(toolbusd, directory, "ledger");
    remotebsp::Client restarted(ipc, discovered.node_id);
    const auto restarted_daemon_id =
        restarted.daemon_identity().instance_id;
    const auto recovered = restarted.runtime_operation_status(
        restarted_daemon_id, "bus-owner", first.operation_id);
    CHECK(recovered.state == remotebsp::RuntimeOperationState::Unknown);
    CHECK(recovered.recovery ==
          remotebsp::RuntimeOperationRecovery::ScopeBlocked);
    CHECK(recovered.replayed);
    expect_ipc_error(
        static_cast<std::uint16_t>(
            remotebsp::toolbusd::IpcErrorCode::LeaseConflict),
        [&] {
            restarted.runtime_control_acquire(
                restarted_daemon_id, filled<16>(0x64U), node_uuid,
                "bus-owner", target_resource, 30000U,
                remotebsp::toolbusd::kRuntimePermissionBusReset);
        });
    std::cerr << "阶段: bus-reset-unknown-restart 结束\n";
}

remotebsp::toolbusd::RuntimeGpioWriteOperation blocked_operation() {
    remotebsp::toolbusd::RuntimeGpioWriteOperation operation;
    operation.daemon_origin = filled<16>(0x11U);
    operation.lease_id = filled<16>(0x22U);
    operation.expected_node_uuid = filled<16>(0x33U);
    operation.owner_key_id = "owner";
    operation.idempotency_key = "blocked";
    operation.permissions = remotebsp::toolbusd::kRuntimePermissionGpioWrite;
    operation.node_id = 1U;
    operation.resource_id = 0x01000000U;
    operation.value = true;
    return operation;
}

void check_startup_block_and_unavailable(const std::string& toolbusd) {
    std::cerr << "阶段: startup-block 启动\n";
    {
        TestDirectory directory;
        remotebsp::toolbusd::OperationDigest operation_id{};
        {
            remotebsp::toolbusd::OperationLedgerOptions options;
            options.directory = directory.child("ledger");
            remotebsp::toolbusd::OperationLedger ledger(options);
            operation_id = ledger.begin_gpio_write(
                blocked_operation()).record.operation_id;
        }
        auto daemon = start_daemon(toolbusd, directory);
        remotebsp::Client client(directory.child("toolbusd.sock").string(), 1U);
        const auto daemon_id = client.daemon_identity().instance_id;
        const auto recovered = client.runtime_operation_status(
            daemon_id, "owner", operation_id);
        CHECK(recovered.state == remotebsp::RuntimeOperationState::Unknown);
        CHECK(recovered.recovery ==
              remotebsp::RuntimeOperationRecovery::ScopeBlocked);
        expect_ipc_error(
            static_cast<std::uint16_t>(
                remotebsp::toolbusd::IpcErrorCode::LeaseConflict),
            [&] {
                client.runtime_control_acquire(
                    daemon_id, filled<16>(0x44U), filled<16>(0x33U),
                    "owner", 0x01000000U, 1000U);
            });
    }

    {
        std::cerr << "阶段: ledger-unavailable 启动\n";
        TestDirectory directory;
        remotebsp::toolbusd::OperationDigest operation_id{};
        {
            remotebsp::toolbusd::OperationLedgerOptions options;
            options.directory = directory.child("ledger");
            remotebsp::toolbusd::OperationLedger ledger(options);
            operation_id = ledger.begin_gpio_write(
                blocked_operation()).record.operation_id;
        }
        const auto segment = directory.child(
            "ledger/segment-0000000000000001.rbol");
        CHECK(::chmod(segment.c_str(), 0644) == 0);
        auto daemon = start_daemon(toolbusd, directory);
        remotebsp::Client client(directory.child("toolbusd.sock").string(), 1U);
        const auto daemon_id = client.daemon_identity().instance_id;
        expect_ipc_error(
            static_cast<std::uint16_t>(
                remotebsp::toolbusd::IpcErrorCode::BackendUnavailable),
            [&] {
                static_cast<void>(client.runtime_operation_status(
                    daemon_id, "owner", operation_id));
            });
        expect_ipc_error(
            static_cast<std::uint16_t>(
                remotebsp::toolbusd::IpcErrorCode::BackendUnavailable),
            [&] {
                client.runtime_control_acquire(
                    daemon_id, filled<16>(0x55U), filled<16>(0x66U),
                    "owner", 0x01000001U, 1000U);
            });
    }

    {
        std::cerr << "阶段: ledger-truncated 启动\n";
        TestDirectory directory;
        remotebsp::toolbusd::OperationDigest operation_id{};
        {
            remotebsp::toolbusd::OperationLedgerOptions options;
            options.directory = directory.child("ledger");
            remotebsp::toolbusd::OperationLedger ledger(options);
            operation_id = ledger.begin_gpio_write(
                blocked_operation()).record.operation_id;
        }
        const auto segment = directory.child(
            "ledger/segment-0000000000000001.rbol");
        const auto committed_size = std::filesystem::file_size(segment);
        CHECK(committed_size > 1U);
        std::filesystem::resize_file(segment, committed_size - 1U);
        CHECK(::chmod(segment.c_str(), 0600) == 0);
        auto daemon = start_daemon(toolbusd, directory);
        remotebsp::Client client(directory.child("toolbusd.sock").string(), 1U);
        const auto daemon_id = client.daemon_identity().instance_id;
        expect_ipc_error(
            static_cast<std::uint16_t>(
                remotebsp::toolbusd::IpcErrorCode::BackendUnavailable),
            [&] {
                static_cast<void>(client.runtime_operation_status(
                    daemon_id, "owner", operation_id));
            });
    }
    std::cerr << "阶段: startup-block/unavailable 结束\n";
}

void check_motion_group_cancel_lease_and_ledger(
    const std::string& toolbusd, const std::string& mock_mcu) {
    std::cerr << "阶段: motion-group-runtime 启动\n";
    TestDirectory directory;
    const auto usb=directory.child("usb.sock").string();
    const auto ipc=directory.child("toolbusd.sock").string();
    auto daemon=start_daemon(toolbusd,directory,"motion-ledger");
    CHECK(wait_for_path(usb,std::chrono::seconds(5)));
    ChildProcess node(spawn({mock_mcu,usb,"usb-mock"}));
    const auto discovered=wait_for_ready_node(ipc,std::chrono::seconds(7));
    remotebsp::Client client(ipc,discovered.node_id);
    const auto daemon_id=client.daemon_identity().instance_id;
    const auto lease=filled<16>(0x79U);

    // 进程级链路首先证明错误 owner 在账本写入前失败关闭。
    client.runtime_motion_group_lease_acquire(
        daemon_id,lease,"motion-owner",7001U,91U,5U,30000U);
    expect_ipc_error(
        static_cast<std::uint16_t>(remotebsp::toolbusd::IpcErrorCode::PermissionDenied),
        [&] { static_cast<void>(client.runtime_motion_group_cancel_operation(
            daemon_id,lease,"wrong-owner","cancel-wrong",7001U,91U,5U,1800U)); });

    const auto outcome=client.runtime_motion_group_cancel_operation(
        daemon_id,lease,"motion-owner","cancel-1",7001U,91U,5U,1800U);
    CHECK(outcome.transaction_id==7001U);
    CHECK(outcome.group_id==91U);
    CHECK(outcome.plan_generation==5U);
    CHECK(outcome.operation.kind==remotebsp::RuntimeOperationKind::MotionGroupCancel);
    CHECK(outcome.operation.state==remotebsp::RuntimeOperationState::Rejected);
    CHECK(outcome.operation.recovery==remotebsp::RuntimeOperationRecovery::NotSent);
    const auto queried=client.runtime_operation_status(
        daemon_id,"motion-owner",outcome.operation.operation_id);
    CHECK(queried.kind==remotebsp::RuntimeOperationKind::MotionGroupCancel);
    CHECK(queried.state==remotebsp::RuntimeOperationState::Rejected);
    CHECK(queried.replayed);
    const auto replay=client.runtime_motion_group_cancel_operation(
        daemon_id,lease,"motion-owner","cancel-1",7001U,91U,5U,1800U);
    CHECK(replay.operation.operation_id==outcome.operation.operation_id);
    CHECK(replay.operation.replayed);
    client.runtime_motion_group_lease_release(daemon_id,lease,"motion-owner");
    std::cerr << "阶段: motion-group-runtime 结束\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::cerr << "用法: toolbusd_operation_ledger_process_tests "
                     "<toolbusd> <mock_mcu> <bus_manifest>\n";
        return 2;
    }
    try {
        check_required_option(argv[1]);
        check_live_routing(argv[1], argv[2]);
        check_concurrent_gpio_replay(argv[1], argv[2]);
        check_gpio_terminal_persistence_failure_flags(argv[1], argv[2]);
        check_release_terminal_persistence_failure_flags(argv[1], argv[2]);
        check_bus_reset_unknown_and_restart(argv[1], argv[2], argv[3]);
        check_motion_group_cancel_lease_and_ledger(argv[1], argv[2]);
        check_startup_block_and_unavailable(argv[1]);
    } catch (const std::exception& error) {
        std::cerr << "进程测试异常: " << error.what() << '\n';
        ++failures;
    }
    if (failures != 0) {
        std::cerr << failures << " 项 toolbusd OperationLedger 进程测试失败\n";
        return 1;
    }
    std::cout << "toolbusd OperationLedger 进程测试通过\n";
    return 0;
}
