"""Regression checks for the minimal BLE runtime policy documented in firmware."""

import re
import unittest
from pathlib import Path


ROOT = Path(__file__).parents[1]
BLE_SOURCE = (ROOT / "src/core/ble/blePeripheral.cpp").read_text(encoding="utf-8")
BLE_HEADER = (ROOT / "src/core/ble/blePeripheral.h").read_text(encoding="utf-8")


class BleRuntimeContractTest(unittest.TestCase):
    def test_runtime_timeouts_are_explicit(self) -> None:
        self.assertIn("INITIAL_ADVERTISING_MS = 60UL * 1000UL", BLE_SOURCE)
        self.assertIn("RECONNECT_ADVERTISING_MS = 30UL * 1000UL", BLE_SOURCE)
        self.assertIn("CONNECTED_INACTIVITY_MS = 120UL * 1000UL", BLE_SOURCE)

    def test_callbacks_only_enqueue_events(self) -> None:
        callbacks = re.search(
            r"class ServerCallbacks.*?\n};\n\nclass StatusCallbacks.*?\n};",
            BLE_SOURCE,
            re.DOTALL,
        )
        self.assertIsNotNone(callbacks)
        callback_source = callbacks.group(0)
        self.assertIn("enqueueEvent", callback_source)
        self.assertNotIn("logInfo", callback_source)
        self.assertNotIn("startAdvertising", callback_source)
        self.assertNotIn("gState =", callback_source)

    def test_future_gatt_activity_hook_is_public(self) -> None:
        self.assertIn("bool noteBleGattActivity();", BLE_HEADER)
        self.assertIn("BleEventType::gattActivity", BLE_SOURCE)

    def test_no_volatile_cross_task_state(self) -> None:
        self.assertNotIn("volatile BleState", BLE_SOURCE)
        self.assertIn("xQueueSend", BLE_SOURCE)
        self.assertIn("xQueueReceive", BLE_SOURCE)


if __name__ == "__main__":
    unittest.main()
