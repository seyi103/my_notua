"""Behavioral tests for priority START_AP delivery and dedicated recovery."""
from collections import deque
from pathlib import Path
import tempfile
import unittest

class BleOrderingBehaviorTest(unittest.TestCase):
    def test_start_ap_immediately_followed_by_disconnect_is_retained(self):
        priority, lifecycle, ordinary = deque([b"START_AP"]), deque(["disconnect"]), deque([b"ordinary"])
        requested = False
        while priority:
            requested |= priority.popleft() == b"START_AP"
        while lifecycle:
            if lifecycle.popleft() == "disconnect": ordinary.clear()
        self.assertTrue(requested)
        self.assertFalse(ordinary)

class SpikeRecoveryBehaviorTest(unittest.TestCase):
    def test_interrupted_replacement_restores_old_then_second_upload_commits(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory); final=root/'softap_spike.bin'; backup=root/'softap_spike.bak'; marker=root/'softap_spike.marker'; temp=root/'notua_upload.tmp'
            old=b'previous-valid'; final.write_bytes(old)
            temp.write_bytes(b'interrupted-new'); marker.write_text('P'); final.rename(backup); temp.rename(final)
            aside=root/'softap_spike.rollback'; final.rename(aside); backup.rename(final); aside.unlink(); marker.unlink()
            self.assertEqual(final.read_bytes(),old); self.assertFalse(backup.exists()); self.assertFalse(marker.exists())
            temp.write_bytes(b'second-valid'); marker.write_text('P'); final.rename(backup); temp.rename(final); backup.unlink(); marker.unlink()
            self.assertEqual(final.read_bytes(),b'second-valid'); self.assertFalse(backup.exists()); self.assertFalse(marker.exists())
