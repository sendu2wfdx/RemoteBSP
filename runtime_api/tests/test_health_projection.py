import struct
import threading
import unittest
import json

from runtime_api.health_projection import (
    HealthProjectionError,
    TrustedNodeHealthProjection,
    TrustedToolbusdHealthProjection,
)
from runtime_api.provider import RuntimeProviderError
from runtime_api.toolbusd_provider import (
    RemoteCliIpcClient,
    ToolbusIpcProtocolError,
    ToolbusdSnapshotProvider,
)


def payload(*, source=3, overall=0, sequence=1, sample_time_ms=100,
            node_id=0, generation=9, metrics=None, reserved=0):
    if metrics is None:
        metrics = [
            (4, 1, 1, 2),
            (15, 2, 1, 0),
            (18, 1, 5, generation),
            (0x8001, 1, 1, 7),
            (0x8005, 1, 0x80, 1234),
        ]
    header = struct.pack("<HBBQQIQHH", 1, source, overall, sequence,
                         sample_time_ms, node_id, generation, len(metrics),
                         reserved)
    return header + b"".join(struct.pack("<HBBQ", *item)
                             for item in metrics)


class HealthProjectionTests(unittest.TestCase):
    def test_node_projection_binds_source_node_and_generation(self):
        registry = TrustedNodeHealthProjection(1, 7, 33)
        wire = payload(source=1, node_id=7, generation=33,
                       metrics=[(1, 2, 3, 0), (6, 1, 1, 2),
                                (7, 1, 1, 8)])
        projected = registry.ingest(wire)
        self.assertEqual(projected["source"], "mcu")
        self.assertEqual(projected["node_id"], 7)
        self.assertIsNone(projected["metrics"][0]["value"])
        self.assertEqual(projected["metrics"][1]["value"], 2)
        self.assertEqual(registry.ingest(wire), projected)
        for invalid in (
                payload(source=2, node_id=7, generation=33),
                payload(source=1, node_id=8, generation=33),
                payload(source=1, node_id=7, generation=34)):
            with self.assertRaises(HealthProjectionError):
                registry.ingest(invalid)

    def test_projects_stable_schema_and_null_for_unavailable(self):
        projection = TrustedToolbusdHealthProjection(9).ingest(payload())
        self.assertEqual(projection, {
            "contract_version": 1,
            "source": "toolbusd",
            "node_id": 0,
            "producer_generation": 9,
            "sample_sequence": 1,
            "sample_time_ms": 100,
            "overall": "unknown",
            "metrics": [
                {"metric_id": 4, "name": "request_queue_depth",
                 "availability": "available", "unit": "count", "value": 2},
                {"metric_id": 15, "name": "tx_frame_total",
                 "availability": "unavailable", "unit": "count",
                 "value": None},
                {"metric_id": 18, "name": "session_generation",
                 "availability": "available", "unit": "generation",
                 "value": 9},
                {"metric_id": 0x8001,
                 "name": "traffic_admitted_packet_total",
                 "availability": "available", "unit": "count", "value": 7},
                {"metric_id": 0x8005,
                 "name": "traffic_estimated_wire_time_ns",
                 "availability": "available", "unit": "nanoseconds",
                 "value": 1234},
            ],
        })

    def test_route_generation_stale_and_equivocation_are_isolated(self):
        registry = TrustedToolbusdHealthProjection(9)
        first_wire = payload(sequence=2, sample_time_ms=200)
        first = registry.ingest(first_wire)
        self.assertEqual(registry.ingest(first_wire), first)

        invalid_cases = (
            payload(source=2, sequence=3),
            payload(node_id=1, sequence=3),
            payload(generation=10, sequence=3,
                    metrics=[(18, 1, 5, 10)]),
            payload(sequence=1),
            payload(sequence=2, metrics=[(4, 1, 1, 99)]),
            payload(sequence=3, sample_time_ms=199),
        )
        for item in invalid_cases:
            with self.subTest(item=item):
                with self.assertRaises(HealthProjectionError):
                    registry.ingest(item)
        accepted = registry.ingest(payload(sequence=3, sample_time_ms=201))
        self.assertEqual(accepted["sample_sequence"], 3)

    def test_decode_failure_does_not_poison_last_good_sample(self):
        registry = TrustedToolbusdHealthProjection(9)
        good = payload(sequence=7)
        expected = registry.ingest(good)
        malformed = good[:-1]
        with self.assertRaises(HealthProjectionError):
            registry.ingest(malformed)
        self.assertEqual(registry.ingest(good), expected)

    def test_generation_switch_invalidates_old_generation(self):
        registry = TrustedToolbusdHealthProjection(9)
        registry.ingest(payload(generation=9, sequence=5))
        registry.replace_generation(10)
        with self.assertRaises(HealthProjectionError):
            registry.ingest(payload(generation=9, sequence=6))
        current = registry.ingest(payload(
            generation=10, sequence=1, metrics=[(18, 1, 5, 10)]))
        self.assertEqual(current["producer_generation"], 10)

    def test_contract_resource_and_semantic_bounds(self):
        registry = TrustedToolbusdHealthProjection(9)
        bad = (
            b"",
            payload(reserved=1),
            payload(metrics=[(4, 1, 1, 3), (4, 1, 1, 3)]),
            payload(metrics=[(4, 2, 1, 1)]),
            payload(metrics=[(4, 1, 2, 1)]),
            payload(metrics=[(4, 1, 1, 3), (5, 1, 1, 2)]),
            payload(metrics=[(1, 1, 3, 1001)]),
            payload(metrics=[(18, 1, 5, 8)]),
            payload(metrics=[(index + 1, 3, 1, 0)
                             for index in range(49)]),
        )
        for item in bad:
            with self.subTest(item=item):
                with self.assertRaises(HealthProjectionError):
                    registry.ingest(item)

    def test_concurrent_duplicate_is_idempotent(self):
        registry = TrustedToolbusdHealthProjection(9)
        wire = payload()
        outputs = []
        errors = []
        lock = threading.Lock()

        def ingest():
            try:
                result = registry.ingest(wire)
                with lock:
                    outputs.append(result)
            except Exception as error:  # pragma: no cover - 仅收集线程异常
                with lock:
                    errors.append(error)

        workers = [threading.Thread(target=ingest) for _ in range(8)]
        for worker in workers:
            worker.start()
        for worker in workers:
            worker.join()
        self.assertEqual(errors, [])
        self.assertEqual(len(outputs), 8)
        self.assertTrue(all(item == outputs[0] for item in outputs))


def health_document(instance="11" * 16, generation=9, sequence=1):
    return json.dumps({
        "schema_version": 1,
        "command": "health-snapshot",
        "data": {
            "ipc_version": 2,
            "daemon_instance_id": instance,
            "ipc": {
                "active_clients": 1, "maximum_clients": 64,
                "peak_clients": 4, "accepted_total": 10,
                "capacity_rejected_total": 2,
                "oversized_frame_total": 1, "timeout_total": 3,
                "thread_creation_failed_total": 0,
            },
            "health": {
                "contract_version": 1,
                "source": 3,
                "overall": 0,
                "sample_sequence": sequence,
                "sample_time_ms": sequence,
                "node_id": 0,
                "producer_generation": generation,
                "metrics": [{
                    "metric_id": 18, "availability": 1,
                    "unit": 5, "value": generation,
                }],
            },
        },
    })


class RuntimeHealthBindingTests(unittest.TestCase):
    def test_remote_cli_json_is_converted_to_strict_wire(self):
        parsed = RemoteCliIpcClient._json_health_snapshot(health_document())
        self.assertEqual(parsed["daemon_instance_id"], "11" * 16)
        self.assertEqual(parsed["producer_generation"], 9)
        self.assertEqual(parsed["ipc"]["maximum_clients"], 64)
        projection = TrustedToolbusdHealthProjection(9).ingest(parsed["wire"])
        self.assertEqual(projection["sample_sequence"], 1)

        bad = json.loads(health_document())
        bad["data"]["health"]["metrics"][0]["value"] = 8
        with self.assertRaises(ToolbusIpcProtocolError):
            RemoteCliIpcClient._json_health_snapshot(json.dumps(bad))

    def test_provider_binds_instance_generation_and_rejects_same_instance_flip(self):
        class Client:
            structured_output = True

            def __init__(self):
                self.responses = []

            def health_snapshot(self):
                return self.responses.pop(0)

        client = Client()
        provider = ToolbusdSnapshotProvider(client)  # type: ignore[arg-type]
        first = RemoteCliIpcClient._json_health_snapshot(
            health_document(sequence=4))
        restarted = RemoteCliIpcClient._json_health_snapshot(
            health_document(instance="22" * 16, generation=10, sequence=1))
        client.responses = [first, restarted]
        self.assertEqual(provider.health_snapshot()["sample_sequence"], 4)
        after = provider.health_snapshot()
        self.assertEqual(after["daemon_instance_id"], "22" * 16)
        self.assertEqual(after["producer_generation"], 10)

        changed = RemoteCliIpcClient._json_health_snapshot(
            health_document(instance="22" * 16, generation=11, sequence=2))
        client.responses = [changed]
        with self.assertRaises(RuntimeProviderError):
            provider.health_snapshot()


if __name__ == "__main__":
    unittest.main()
