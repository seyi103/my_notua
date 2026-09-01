package com.example.notua_app

import android.Manifest
import android.app.Activity
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.*
import android.content.pm.PackageManager
import android.net.*
import android.net.wifi.WifiInfo
import android.net.wifi.WifiNetworkSpecifier
import android.net.wifi.WifiManager
import android.os.*
import android.util.Log
import androidx.core.app.ActivityCompat
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodChannel
import java.io.*
import java.net.HttpURLConnection
import java.util.UUID
import org.json.JSONObject
import java.util.zip.CRC32
import java.util.concurrent.atomic.AtomicBoolean

class MainActivity : FlutterActivity() {
    private class OnceResult(private val delegate: MethodChannel.Result) {
        private val completed = AtomicBoolean(false)
        fun success(value: Any?) { if (completed.compareAndSet(false, true)) delegate.success(value) }
        fun error(code: String, message: String) { if (completed.compareAndSet(false, true)) delegate.error(code, message, null) }
    }
    private val methods = "notua/softap"
    private val events = "notua/softap_progress"
    private val serviceUuid = UUID.fromString("7d2a4b70-8e67-4d8b-9f3a-36c89e210001")
    private val controlUuid = UUID.fromString("7d2a4b70-8e67-4d8b-9f3a-36c89e210003")
    private val infoUuid = UUID.fromString("7d2a4b70-8e67-4d8b-9f3a-36c89e210007")
    private var sink: EventChannel.EventSink? = null
    private var pendingPicker: OnceResult? = null
    private var network: Network? = null
    private var callback: ConnectivityManager.NetworkCallback? = null
    private var activeGatt: BluetoothGatt? = null
    private var activeConnection: HttpURLConnection? = null
    private val handler = Handler(Looper.getMainLooper())
    private var pendingConnect: OnceResult? = null
    private var scanCallback: ScanCallback? = null
    private val logTag = "NotuaSoftAP"

    override fun configureFlutterEngine(engine: FlutterEngine) {
        super.configureFlutterEngine(engine)
        EventChannel(engine.dartExecutor.binaryMessenger, events).setStreamHandler(object: EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, eventSink: EventChannel.EventSink) { sink = eventSink }
            override fun onCancel(arguments: Any?) { sink = null }
        })
        MethodChannel(engine.dartExecutor.binaryMessenger, methods).setMethodCallHandler { call, result ->
            when (call.method) {
                "selectFile" -> { pendingPicker = OnceResult(result); startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type="application/octet-stream"; addCategory(Intent.CATEGORY_OPENABLE) }, 44) }
                "connect" -> connectBle(OnceResult(result))
                "upload" -> upload(call.argument<String>("path")!!, OnceResult(result))
                "cancel" -> { cleanupAll(); result.success(null) }
                else -> result.notImplemented()
            }
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 44) return
        val out = File(cacheDir, "notua-selected.bin")
        if (resultCode != Activity.RESULT_OK || data?.data == null) pendingPicker?.error("cancelled", "No file selected")
        else try { contentResolver.openInputStream(data.data!!).use { input -> FileOutputStream(out).use { input!!.copyTo(it) } }; pendingPicker?.success(out.absolutePath) }
        catch (e: Exception) { pendingPicker?.error("copy", e.message ?: "File copy failed") }
        pendingPicker = null
    }

    private fun connectBle(result: OnceResult) {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) permissions += listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        if (Build.VERSION.SDK_INT in 29..32) permissions += Manifest.permission.ACCESS_FINE_LOCATION
        if (Build.VERSION.SDK_INT >= 33) permissions += Manifest.permission.NEARBY_WIFI_DEVICES
        if (permissions.any { ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }) {
            ActivityCompat.requestPermissions(this, permissions.toTypedArray(), 91)
            result.error("permissions", "Grant nearby device permissions, then tap Connect again"); return
        }
        pendingConnect = result
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        val scanner = adapter.bluetoothLeScanner
        val scan = object: ScanCallback() {
            override fun onScanResult(type: Int, found: ScanResult) {
                if (found.device.name != "Notua" || pendingConnect == null) return
                stopScanSafely(this); scanCallback = null; handler.removeCallbacksAndMessages("ble-scan")
                activeGatt = found.device.connectGatt(this@MainActivity, false, object: BluetoothGattCallback() {
                    override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, state: Int) {
                        if (status != BluetoothGatt.GATT_SUCCESS) failBle("GATT connection failed: $status")
                        else if (state == BluetoothProfile.STATE_CONNECTED) gatt.discoverServices()
                        else if (state == BluetoothProfile.STATE_DISCONNECTED && pendingConnect != null) failBle("GATT disconnected")
                    }
                    override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
                        if (status != BluetoothGatt.GATT_SUCCESS) failBle("Service discovery failed: $status")
                        else gatt.readCharacteristic(gatt.getService(serviceUuid)?.getCharacteristic(infoUuid) ?: run { failBle("AP info characteristic missing"); return })
                    }
                    @Deprecated("API 33 compatibility")
                    override fun onCharacteristicRead(gatt: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
                        if (Build.VERSION.SDK_INT < 33) handleInfoRead(gatt, c, c.value, status)
                    }
                    override fun onCharacteristicRead(gatt: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
                        handleInfoRead(gatt, c, value, status)
                    }
                    override fun onCharacteristicWrite(gatt: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
                        if (c.uuid != controlUuid) return
                        handler.removeCallbacksAndMessages("gatt-write")
                        if (status != BluetoothGatt.GATT_SUCCESS) failBle("START_AP write failed: $status")
                        else {
                            val complete = pendingConnect; pendingConnect = null
                            gatt.disconnect(); gatt.close(); activeGatt = null
                            runOnUiThread { complete?.success(getSharedPreferences("notua", MODE_PRIVATE).getString("json", "")) }
                        }
                    }
                })
            }
            override fun onScanFailed(errorCode: Int) { failBle("BLE scan failed: $errorCode") }
        }
        scanCallback = scan
        try { scanner.startScan(scan) } catch (e: SecurityException) { scanCallback=null; failBle("BLE scan permission denied: ${e.message}"); return }
        handler.postAtTime({ if (pendingConnect != null) { stopScanSafely(scan); scanCallback=null; failBle("BLE scan timed out") } }, "ble-scan", SystemClock.uptimeMillis()+15_000)
    }

    private fun handleInfoRead(gatt: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray, status: Int) {
        if (c.uuid != infoUuid || status != BluetoothGatt.GATT_SUCCESS) { failBle("Unable to read AP info"); return }
        try {
            val json=String(value, Charsets.UTF_8); val info=JSONObject(json)
            getSharedPreferences("notua", MODE_PRIVATE).edit().putString("json",json).putString("ssid",info.getString("ssid")).putString("password",info.getString("password")).putString("ip",info.getString("ip")).putInt("port",info.getInt("port")).putInt("candidateSlot",info.getInt("candidateSlot")).apply()
            if (info.getInt("candidateSlot") < 0) {
                val complete=pendingConnect; pendingConnect=null; gatt.disconnect(); gatt.close(); activeGatt=null
                runOnUiThread { complete?.success(json) }; return
            }
            val control=gatt.getService(serviceUuid).getCharacteristic(controlUuid); val command="START_AP".toByteArray()
            val started = if (Build.VERSION.SDK_INT >= 33) gatt.writeCharacteristic(control, command, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
                else { @Suppress("DEPRECATION") control.value=command; @Suppress("DEPRECATION") gatt.writeCharacteristic(control) }
            if (!started) { failBle("Unable to start START_AP write"); return }
            handler.postAtTime({ if(pendingConnect != null) failBle("START_AP write timed out") }, "gatt-write", SystemClock.uptimeMillis()+5_000)
        } catch(e:Exception) { failBle("Invalid AP info: ${e.message}") }
    }

    private fun stopScanSafely(scan: ScanCallback) {
        try { getSystemService(BluetoothManager::class.java).adapter.bluetoothLeScanner.stopScan(scan) }
        catch (_: SecurityException) { }
    }

    private fun failBle(message:String) {
        handler.removeCallbacksAndMessages("ble-scan"); handler.removeCallbacksAndMessages("gatt-write")
        activeGatt?.disconnect(); activeGatt?.close(); activeGatt=null
        val result=pendingConnect; pendingConnect=null; runOnUiThread { result?.error("ble",message) }
    }

    private fun emitStatus(message: String) {
        Log.i(logTag, message)
        runOnUiThread { sink?.success(mapOf("type" to "status", "message" to message)) }
    }

    private fun normalizedSsid(value: String?): String? {
        if (value == null || value == WifiManager.UNKNOWN_SSID) return null
        return value.removeSurrounding("\"")
    }

    private fun findExistingNotuaNetwork(cm: ConnectivityManager, expectedSsid: String): Network? {
        return cm.allNetworks.firstOrNull { candidate ->
            val capabilities = cm.getNetworkCapabilities(candidate) ?: return@firstOrNull false
            if (!capabilities.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) return@firstOrNull false
            val wifiInfo = capabilities.transportInfo as? WifiInfo ?: return@firstOrNull false
            normalizedSsid(wifiInfo.ssid) == expectedSsid
        }
    }

    private fun upload(path: String, result: OnceResult) = Thread {
        val file = File(path)
        if (file.length() != 1_920_000L) { runOnUiThread { result.error("size", "File must be exactly 1,920,000 bytes") }; return@Thread }
        val crc = CRC32(); FileInputStream(file).use { input -> val b=ByteArray(64*1024); while (true) { val n=input.read(b); if(n<0) break; crc.update(b,0,n) } }
        val cm = getSystemService(ConnectivityManager::class.java)
        network = null
        val info = getSharedPreferences("notua", MODE_PRIVATE)
        val ssid=info.getString("ssid", null)
        val slot=info.getInt("candidateSlot", -1)
        if (ssid == null || slot < 0) { runOnUiThread { result.error("connect", "Connect to Notua first") }; return@Thread }
        val existing = try { findExistingNotuaNetwork(cm, ssid) }
        catch (e: SecurityException) {
            runOnUiThread { result.error("wifi", "Unable to inspect Wi-Fi networks: ${e.message}") }
            return@Thread
        }
        if (existing != null) {
            network = existing
            emitStatus("Existing Notua network reused: $ssid (app-owned=false)")
            emitStatus("Notua network available: $ssid (app-owned=false)")
            verifyAndUpload(existing, file, slot, crc.value, result, cm, null)
            return@Thread
        }
        val spec=WifiNetworkSpecifier.Builder().setSsid(ssid).setWpa2Passphrase(info.getString("password", "")!!).build()
        val request=NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).setNetworkSpecifier(spec).build()
        val acceptedNetwork = AtomicBoolean(false)
        val requestedCallback=object: ConnectivityManager.NetworkCallback() {
            override fun onAvailable(n: Network) {
                if (!acceptedNetwork.compareAndSet(false, true)) return
                handler.removeCallbacksAndMessages("wifi")
                network=n
                emitStatus("Notua network available: $ssid (app-owned=true)")
                verifyAndUpload(n,file,slot,crc.value,result,cm,this)
            }
            override fun onUnavailable() {
                if (acceptedNetwork.get()) {
                    Log.w(logTag, "Ignoring unavailable callback after a Notua network was selected")
                    return
                }
                emitStatus("Automatic Notua network request unavailable")
                cleanupNetwork(cm, this)
                runOnUiThread { result.error("wifi", "Notua network unavailable") }
            }
            override fun onLost(lost: Network) {
                Log.w(logTag, "Requested network changed state; deferring cleanup to active operation")
            }
        }
        callback=requestedCallback
        runOnUiThread {
            try {
                emitStatus("Automatic Notua network requested: $ssid")
                cm.requestNetwork(request, requestedCallback)
                handler.postAtTime({ if(network==null){ cleanupNetwork(cm, requestedCallback); result.error("wifi","Notua network request timed out") } }, "wifi", SystemClock.uptimeMillis()+20_000)
            } catch (e: SecurityException) { cleanupNetwork(cm, requestedCallback); result.error("wifi", "Wi-Fi permission denied: ${e.message}") }
        }
    }.start()

    private fun verifyAndUpload(n: Network, file: File, slot:Int, crc:Long, result:OnceResult, cm:ConnectivityManager, ownedCallback:ConnectivityManager.NetworkCallback?)=Thread {
        var healthPending = true
        try {
            val prefs=getSharedPreferences("notua", MODE_PRIVATE); val ip=prefs.getString("ip","192.168.4.1"); val port=prefs.getInt("port",80)
            val health=n.openConnection(java.net.URL("http://$ip:$port/health")) as HttpURLConnection
            activeConnection=health
            health.requestMethod="GET"; health.connectTimeout=5_000; health.readTimeout=5_000
            val healthCode=health.responseCode
            health.disconnect(); activeConnection=null
            if (healthCode !in 200..299) throw IOException("Health check returned HTTP $healthCode")
            emitStatus("Notua health check passed (HTTP $healthCode)")
            healthPending = false
            emitStatus("Upload started for candidate slot $slot")
            val connection=n.openConnection(java.net.URL("http://$ip:$port/images/$slot")) as HttpURLConnection
            activeConnection=connection; connection.requestMethod="PUT"; connection.doOutput=true; connection.setFixedLengthStreamingMode(file.length()); connection.setRequestProperty("X-Notua-CRC32", "%08x".format(crc)); connection.connectTimeout=10000; connection.readTimeout=30000
            val start=SystemClock.elapsedRealtime(); var sent=0L; var prior=start; var priorBytes=0L
            FileInputStream(file).use { input -> connection.outputStream.use { output -> val b=ByteArray(16*1024); while(true){ val count=input.read(b); if(count<0)break; output.write(b,0,count); sent+=count; val now=SystemClock.elapsedRealtime(); if(now-prior>=250){ val instant=(sent-priorBytes)*1000.0/(now-prior)/1024; val average=sent*1000.0/(now-start)/1024; runOnUiThread { sink?.success(mapOf("sent" to sent,"total" to file.length(),"instantKiBs" to instant,"averageKiBs" to average,"elapsedMs" to now-start)) }; prior=now; priorBytes=sent } } } }
            val finished=SystemClock.elapsedRealtime(); val average=sent*1000.0/(finished-start)/1024
            runOnUiThread { sink?.success(mapOf("sent" to sent,"total" to file.length(),"instantKiBs" to average,"averageKiBs" to average,"elapsedMs" to finished-start)) }
            val body=(if(connection.responseCode<400) connection.inputStream else connection.errorStream).bufferedReader().readText()
            emitStatus("Upload completed (HTTP ${connection.responseCode})")
            runOnUiThread { result.success(mapOf("httpStatus" to connection.responseCode,"body" to body,"crc32" to "%08x".format(crc))) }
        } catch(e:Exception){
            val message = e.message ?: "Upload failed"
            val displayMessage = if (message.contains("cleartext", ignoreCase = true))
                "Android blocked cleartext HTTP. Install a debug build with the Notua SoftAP cleartext policy enabled."
            else message
            emitStatus(if (healthPending) "Notua health check failed: $displayMessage" else "Upload failed: $displayMessage")
            runOnUiThread { result.error("upload", displayMessage) }
        }
        finally { activeConnection=null; cleanupNetwork(cm, ownedCallback) }
    }.start()
    private fun cleanupNetwork(cm: ConnectivityManager = getSystemService(ConnectivityManager::class.java), ownedCallback: ConnectivityManager.NetworkCallback? = callback) {
        handler.removeCallbacksAndMessages("wifi"); activeConnection?.disconnect(); activeConnection=null
        if (ownedCallback != null) {
            try { cm.unregisterNetworkCallback(ownedCallback) } catch (_:Exception) {}
            if (callback === ownedCallback) callback=null
            emitStatus("Network cleanup complete (app-owned=true; callback unregistered)")
        } else {
            emitStatus("Network cleanup complete (app-owned=false; existing Wi-Fi retained)")
        }
        network=null
    }
    private fun cleanupAll() {
        pendingPicker?.error("cancelled","Cancelled"); pendingPicker=null
        scanCallback?.let { try { getSystemService(BluetoothManager::class.java).adapter.bluetoothLeScanner.stopScan(it) } catch (_:Exception) {} }; scanCallback=null
        failBle("Cancelled"); cleanupNetwork()
    }
    override fun onDestroy() { cleanupAll(); super.onDestroy() }

}
