package com.example.notua_app

import android.Manifest
import android.app.Activity
import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.content.*
import android.content.pm.PackageManager
import android.net.*
import android.net.wifi.WifiNetworkSpecifier
import android.os.*
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

class MainActivity : FlutterActivity() {
    private val methods = "notua/softap"
    private val events = "notua/softap_progress"
    private val serviceUuid = UUID.fromString("7d2a4b70-8e67-4d8b-9f3a-36c89e210001")
    private val controlUuid = UUID.fromString("7d2a4b70-8e67-4d8b-9f3a-36c89e210003")
    private val infoUuid = UUID.fromString("7d2a4b70-8e67-4d8b-9f3a-36c89e210007")
    private var sink: EventChannel.EventSink? = null
    private var pendingPicker: MethodChannel.Result? = null
    private var network: Network? = null
    private var callback: ConnectivityManager.NetworkCallback? = null

    override fun configureFlutterEngine(engine: FlutterEngine) {
        super.configureFlutterEngine(engine)
        EventChannel(engine.dartExecutor.binaryMessenger, events).setStreamHandler(object: EventChannel.StreamHandler {
            override fun onListen(arguments: Any?, eventSink: EventChannel.EventSink) { sink = eventSink }
            override fun onCancel(arguments: Any?) { sink = null }
        })
        MethodChannel(engine.dartExecutor.binaryMessenger, methods).setMethodCallHandler { call, result ->
            when (call.method) {
                "selectFile" -> { pendingPicker = result; startActivityForResult(Intent(Intent.ACTION_OPEN_DOCUMENT).apply { type="application/octet-stream"; addCategory(Intent.CATEGORY_OPENABLE) }, 44) }
                "connect" -> connectBle(result)
                "upload" -> upload(call.argument<String>("path")!!, call.argument<Int>("slot") ?: 0, result)
                else -> result.notImplemented()
            }
        }
    }

    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != 44) return
        val out = File(cacheDir, "notua-selected.bin")
        if (resultCode != Activity.RESULT_OK || data?.data == null) pendingPicker?.error("cancelled", "No file selected", null)
        else try { contentResolver.openInputStream(data.data!!).use { input -> FileOutputStream(out).use { input!!.copyTo(it) } }; pendingPicker?.success(out.absolutePath) }
        catch (e: Exception) { pendingPicker?.error("copy", e.message, null) }
        pendingPicker = null
    }

    private fun connectBle(result: MethodChannel.Result) {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) permissions += listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        if (Build.VERSION.SDK_INT >= 33) permissions += Manifest.permission.NEARBY_WIFI_DEVICES
        if (permissions.any { ActivityCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED }) {
            ActivityCompat.requestPermissions(this, permissions.toTypedArray(), 91)
            result.error("permissions", "Grant nearby device permissions, then tap Connect again", null); return
        }
        val adapter = getSystemService(BluetoothManager::class.java).adapter
        var returned = false
        adapter.bluetoothLeScanner.startScan(object: ScanCallback() {
            override fun onScanResult(type: Int, scan: ScanResult) {
                if (returned || scan.device.name != "Notua") return
                returned = true; adapter.bluetoothLeScanner.stopScan(this)
                scan.device.connectGatt(this@MainActivity, false, object: BluetoothGattCallback() {
                    override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, state: Int) { if (state == BluetoothProfile.STATE_CONNECTED) gatt.discoverServices() }
                    override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) { gatt.readCharacteristic(gatt.getService(serviceUuid).getCharacteristic(infoUuid)) }
                    @Deprecated("Deprecated in API 33") override fun onCharacteristicRead(gatt: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
                        if (c.uuid != infoUuid || status != BluetoothGatt.GATT_SUCCESS) { result.error("ble", "Unable to read AP info", null); gatt.close(); return }
                        val json = String(c.value, Charsets.UTF_8)
                        val info = JSONObject(json)
                        getSharedPreferences("notua", MODE_PRIVATE).edit().putString("ssid", info.getString("ssid")).putString("password", info.getString("password")).putString("ip", info.getString("ip")).putInt("port", info.getInt("port")).apply()
                        val control = gatt.getService(serviceUuid).getCharacteristic(controlUuid); control.value = "START_AP".toByteArray(); gatt.writeCharacteristic(control)
                        runOnUiThread { result.success(json) }; gatt.disconnect(); gatt.close()
                    }
                })
            }
        })
    }

    private fun upload(path: String, slot: Int, result: MethodChannel.Result) = Thread {
        val file = File(path)
        if (file.length() != 1_920_000L) { runOnUiThread { result.error("size", "File must be exactly 1,920,000 bytes", null) }; return@Thread }
        val crc = CRC32(); FileInputStream(file).use { input -> val b=ByteArray(64*1024); while (true) { val n=input.read(b); if(n<0) break; crc.update(b,0,n) } }
        val cm = getSystemService(ConnectivityManager::class.java)
        val info = getSharedPreferences("notua", MODE_PRIVATE)
        val ssid=info.getString("ssid", null)
        if (ssid == null) { runOnUiThread { result.error("connect", "Connect to Notua first", null) }; return@Thread }
        val spec=WifiNetworkSpecifier.Builder().setSsid(ssid).setWpa2Passphrase(info.getString("password", "")!!).build()
        val request=NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI).removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).setNetworkSpecifier(spec).build()
        callback=object: ConnectivityManager.NetworkCallback() {
            override fun onAvailable(n: Network) { network=n; performUpload(n,file,slot,crc.value,result,cm,this) }
            override fun onUnavailable() { runOnUiThread { result.error("wifi", "Notua network unavailable", null) } }
        }
        runOnUiThread { cm.requestNetwork(request, callback!!) }
    }.start()

    private fun performUpload(n: Network, file: File, slot:Int, crc:Long, result:MethodChannel.Result, cm:ConnectivityManager, cb:ConnectivityManager.NetworkCallback)=Thread {
        try {
            val prefs=getSharedPreferences("notua", MODE_PRIVATE); val ip=prefs.getString("ip","192.168.4.1"); val port=prefs.getInt("port",80)
            val connection=n.openConnection(java.net.URL("http://$ip:$port/images/$slot")) as HttpURLConnection
            connection.requestMethod="PUT"; connection.doOutput=true; connection.setFixedLengthStreamingMode(file.length()); connection.setRequestProperty("X-Notua-CRC32", "%08x".format(crc)); connection.connectTimeout=10000; connection.readTimeout=30000
            val start=SystemClock.elapsedRealtime(); var sent=0L; var prior=start; var priorBytes=0L
            FileInputStream(file).use { input -> connection.outputStream.use { output -> val b=ByteArray(16*1024); while(true){ val count=input.read(b); if(count<0)break; output.write(b,0,count); sent+=count; val now=SystemClock.elapsedRealtime(); if(now-prior>=250){ val instant=(sent-priorBytes)*1000.0/(now-prior)/1024; val average=sent*1000.0/(now-start)/1024; sink?.success(mapOf("sent" to sent,"total" to file.length(),"instantKiBs" to instant,"averageKiBs" to average,"elapsedMs" to now-start)); prior=now; priorBytes=sent } } } }
            val body=(if(connection.responseCode<400) connection.inputStream else connection.errorStream).bufferedReader().readText()
            runOnUiThread { result.success(mapOf("httpStatus" to connection.responseCode,"body" to body,"crc32" to "%08x".format(crc))) }
        } catch(e:Exception){ runOnUiThread { result.error("upload",e.message,null) } }
        finally { cm.unregisterNetworkCallback(cb); network=null; callback=null }
    }.start()
}
