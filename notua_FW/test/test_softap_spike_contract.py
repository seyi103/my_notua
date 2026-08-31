"""Regression contracts for the explicitly build-gated hardware spike."""
import unittest
from pathlib import Path
ROOT=Path(__file__).parents[1]
BLE=(ROOT/'src/core/ble/blePeripheral.cpp').read_text(); AP=(ROOT/'src/core/network/softApTransfer.cpp').read_text(); STORAGE=(ROOT/'src/core/storage/imageStorage.cpp').read_text(); MAIN=(ROOT/'src/main.cpp').read_text(); ANDROID=(ROOT.parent/'notua_app/android/app/src/main/kotlin/com/example/notua_app/MainActivity.kt').read_text(); DART=(ROOT.parent/'notua_app/lib/main.dart').read_text(); PIO=(ROOT/'platformio.ini').read_text()
class SoftApContract(unittest.TestCase):
 def test_start_ap_is_queued(self):
  callback=BLE.split('class ControlCallbacks',1)[1].split('\n};',1)[0]; self.assertIn('enqueueTransfer',callback); self.assertNotIn('requestSoftApStart',callback)
 def test_release_is_gated(self):
  self.assertIn('#if NOTUA_SOFTAP_HTTP_SPIKE\nconstexpr const char* SOFTAP_INFO_UUID',BLE); self.assertIn('build_src_filter = +<*> -<core/network/softApTransfer.cpp>',PIO)
 def test_gate_precedes_storage_recovery(self):
  request=AP.split('void acceptRequest()',1)[1].split('\n}',1)[0]; self.assertLess(request.index('acquireTransferSession'),request.index('storage.startSpike'))
  startup=AP.split('void pollSoftApTransfer()',1)[1].split('if (!running)',1)[0]; self.assertNotIn('storage.begin',startup)
 def test_candidate_and_lifecycle(self):
  self.assertIn('"all-slots-active"',AP); self.assertIn('candidateSlot',AP); self.assertIn('softApTransferOwnsLifecycle()',MAIN); self.assertNotIn("'slot':0",DART); self.assertIn('Selected unused candidate slot',DART)
 def test_dedicated_second_upload_and_power_loss_recovery(self):
  for token in ['SPIKE_BACKUP_PATH','SPIKE_MARKER_PATH','recoverSpike()','spike_ ? String("P")']:
   self.assertIn(token,STORAGE)
  marker=STORAGE.split('const String markerValue',1)[1].split(';',1)[0]; self.assertIn('spike_ ? String("P") : String("P") + slot_',marker)
  self.assertIn('SyncStage::prepared',STORAGE); self.assertIn('SyncStage::committed',STORAGE)
 def test_http_and_android_cleanup(self):
  for token in ['length != IMAGE_BYTES','startSpike','storage.abort()','"upload-timeout"','releaseTransferSession']: self.assertIn(token,AP)
  for token in ['ACCESS_FINE_LOCATION','SecurityException','onCharacteristicWrite','START_AP write timed out','onScanFailed','Notua network request timed out','unregisterNetworkCallback','OnceResult','"sent" to sent']: self.assertIn(token,ANDROID)
if __name__=='__main__': unittest.main()
