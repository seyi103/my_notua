"""Static contracts for the explicitly build-gated hardware spike."""
import unittest
from pathlib import Path
ROOT=Path(__file__).parents[1]
BLE=(ROOT/'src/core/ble/blePeripheral.cpp').read_text()
AP=(ROOT/'src/core/network/softApTransfer.cpp').read_text()
ANDROID=(ROOT.parent/'notua_app/android/app/src/main/kotlin/com/example/notua_app/MainActivity.kt').read_text()
PIO=(ROOT/'platformio.ini').read_text()
class SoftApContract(unittest.TestCase):
 def test_start_ap_is_queued_and_processed_outside_callback(self):
  callback=BLE.split('class ControlCallbacks',1)[1].split('\n};',1)[0]
  self.assertIn('enqueueTransfer',callback); self.assertNotIn('requestSoftApStart',callback)
  self.assertIn('memcmp(transfer.bytes, "START_AP"',BLE)
 def test_info_is_precomputed_and_release_gatt_is_gated(self):
  self.assertNotIn('class SoftApInfoCallbacks',BLE)
  self.assertIn('#if NOTUA_SOFTAP_HTTP_SPIKE\nconstexpr const char* SOFTAP_INFO_UUID',BLE)
  self.assertIn('build_src_filter = +<*> -<core/network/softApTransfer.cpp>',PIO)
 def test_http_validation_exclusivity_and_cleanup(self):
  for token in ['length != IMAGE_BYTES','"active-slot"','"busy"','startSpike','storage.abort()','"upload-timeout"','releaseTransferSession']:
   self.assertIn(token,AP)
 def test_android_waits_for_write_and_has_timeouts(self):
  for token in ['onCharacteristicWrite','START_AP write timed out','BluetoothStatusCodes.SUCCESS','onScanFailed','BLE scan timed out','Notua network request timed out','unregisterNetworkCallback','runOnUiThread { sink?.success']:
   self.assertIn(token,ANDROID)
  write=ANDROID.split('onCharacteristicWrite',1)[1].split('private fun upload',1)[0]
  self.assertIn('gatt.disconnect()',write)
if __name__=='__main__': unittest.main()
