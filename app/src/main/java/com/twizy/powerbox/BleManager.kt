package com.twizy.powerbox

import android.bluetooth.*
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import org.json.JSONObject
import java.util.UUID

data class PowerBoxData(
    val voltage: Double = 0.0,
    val current: Double = 0.0,
    val power: Double = 0.0,
    val soc: Int = 0,
    val relay1: Boolean = false,
    val relay2: Boolean = false
)

enum class ConnectionState { DISCONNECTED, SCANNING, CONNECTING, CONNECTED }

data class ScannedDevice(val name: String, val address: String)

class BleManager(private val context: Context) {

    companion object {
        private val NUS_SERVICE = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        private val NUS_TX_CHAR = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
        private val NUS_RX_CHAR = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
        private val CCCD_UUID = UUID.fromString("00002902-0000-1000-8000-00805F9B34FB")
    }

    private val bluetoothManager = context.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val bluetoothAdapter get() = bluetoothManager.adapter

    private var gatt: BluetoothGatt? = null
    private var rxCharacteristic: BluetoothGattCharacteristic? = null

    private val _connectionState = MutableStateFlow(ConnectionState.DISCONNECTED)
    val connectionState: StateFlow<ConnectionState> = _connectionState

    private val _powerBoxData = MutableStateFlow(PowerBoxData())
    val powerBoxData: StateFlow<PowerBoxData> = _powerBoxData

    private val _scannedDevices = MutableStateFlow<List<ScannedDevice>>(emptyList())
    val scannedDevices: StateFlow<List<ScannedDevice>> = _scannedDevices

    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, result: ScanResult) {
            val name = result.device.name ?: return
            val address = result.device.address
            val current = _scannedDevices.value
            if (current.none { it.address == address }) {
                _scannedDevices.value = current + ScannedDevice(name, address)
            }
        }
    }

    fun startScan() {
        _scannedDevices.value = emptyList()
        _connectionState.value = ConnectionState.SCANNING
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .build()
        bluetoothAdapter.bluetoothLeScanner?.startScan(null, settings, scanCallback)
    }

    fun stopScan() {
        bluetoothAdapter.bluetoothLeScanner?.stopScan(scanCallback)
        if (_connectionState.value == ConnectionState.SCANNING) {
            _connectionState.value = ConnectionState.DISCONNECTED
        }
    }

    fun connect(address: String) {
        stopScan()
        _connectionState.value = ConnectionState.CONNECTING
        val device = bluetoothAdapter.getRemoteDevice(address)
        gatt = device.connectGatt(context, false, gattCallback, BluetoothDevice.TRANSPORT_LE)
    }

    fun disconnect() {
        gatt?.disconnect()
        gatt?.close()
        gatt = null
        rxCharacteristic = null
        _connectionState.value = ConnectionState.DISCONNECTED
    }

    fun sendCommand(json: String) {
        val char = rxCharacteristic ?: return
        val g = gatt ?: return
        @Suppress("DEPRECATION")
        char.value = json.toByteArray(Charsets.UTF_8)
        @Suppress("DEPRECATION")
        g.writeCharacteristic(char)
    }

    private val gattCallback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(gatt: BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                BluetoothProfile.STATE_CONNECTED -> {
                    _connectionState.value = ConnectionState.CONNECTED
                    gatt.discoverServices()
                }
                BluetoothProfile.STATE_DISCONNECTED -> {
                    _connectionState.value = ConnectionState.DISCONNECTED
                    gatt.close()
                    this@BleManager.gatt = null
                }
            }
        }

        override fun onServicesDiscovered(gatt: BluetoothGatt, status: Int) {
            val service = gatt.getService(NUS_SERVICE) ?: return
            rxCharacteristic = service.getCharacteristic(NUS_RX_CHAR)
            val txChar = service.getCharacteristic(NUS_TX_CHAR) ?: return
            gatt.setCharacteristicNotification(txChar, true)
            val descriptor = txChar.getDescriptor(CCCD_UUID)
            @Suppress("DEPRECATION")
            descriptor?.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
            @Suppress("DEPRECATION")
            descriptor?.let { gatt.writeDescriptor(it) }
        }

        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(gatt: BluetoothGatt, characteristic: BluetoothGattCharacteristic) {
            if (characteristic.uuid == NUS_TX_CHAR) {
                val raw = characteristic.value?.toString(Charsets.UTF_8) ?: return
                parsePowerBoxData(raw)
            }
        }
    }

    private fun parsePowerBoxData(raw: String) {
        try {
            val json = JSONObject(raw)
            _powerBoxData.value = PowerBoxData(
                voltage = json.optDouble("v", 0.0),
                current = json.optDouble("i", 0.0),
                power = json.optDouble("p", 0.0),
                soc = json.optInt("soc", 0),
                relay1 = json.optBoolean("r1", false),
                relay2 = json.optBoolean("r2", false)
            )
        } catch (_: Exception) { }
    }
}
