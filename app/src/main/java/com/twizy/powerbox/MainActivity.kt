package com.twizy.powerbox

import android.Manifest
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothManager
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Build
import android.os.Bundle
import android.widget.Toast
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import androidx.lifecycle.lifecycleScope
import androidx.recyclerview.widget.LinearLayoutManager
import com.twizy.powerbox.databinding.ActivityMainBinding
import kotlinx.coroutines.launch

class MainActivity : AppCompatActivity() {

    private lateinit var binding: ActivityMainBinding
    private lateinit var bleManager: BleManager
    private lateinit var adapter: DeviceAdapter

    private val requiredPermissions = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        arrayOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
    } else {
        arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    private val permissionLauncher = registerForActivityResult(
        ActivityResultContracts.RequestMultiplePermissions()
    ) { grants ->
        if (grants.values.all { it }) startScan()
        else Toast.makeText(this, R.string.permissions_required, Toast.LENGTH_LONG).show()
    }

    private val enableBtLauncher = registerForActivityResult(
        ActivityResultContracts.StartActivityForResult()
    ) { if (it.resultCode == RESULT_OK) checkPermissionsAndScan() }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityMainBinding.inflate(layoutInflater)
        setContentView(binding.root)

        bleManager = BleManager(applicationContext)

        adapter = DeviceAdapter { device ->
            bleManager.stopScan()
            val intent = Intent(this, DashboardActivity::class.java).apply {
                putExtra(DashboardActivity.EXTRA_ADDRESS, device.address)
                putExtra(DashboardActivity.EXTRA_NAME, device.name)
            }
            startActivity(intent)
        }

        binding.recyclerDevices.layoutManager = LinearLayoutManager(this)
        binding.recyclerDevices.adapter = adapter

        binding.fabScan.setOnClickListener { checkPermissionsAndScan() }

        lifecycleScope.launch {
            bleManager.scannedDevices.collect { devices ->
                adapter.submitList(devices.toList())
            }
        }

        lifecycleScope.launch {
            bleManager.connectionState.collect { state ->
                binding.textScanStatus.text = when (state) {
                    ConnectionState.SCANNING -> getString(R.string.scanning)
                    ConnectionState.DISCONNECTED -> getString(R.string.tap_scan)
                    else -> ""
                }
            }
        }
    }

    private fun checkPermissionsAndScan() {
        val bluetoothManager = getSystemService(BluetoothManager::class.java)
        val bluetoothAdapter = bluetoothManager?.adapter
        if (bluetoothAdapter == null || !bluetoothAdapter.isEnabled) {
            enableBtLauncher.launch(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE))
            return
        }
        val missing = requiredPermissions.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) startScan() else permissionLauncher.launch(missing.toTypedArray())
    }

    private fun startScan() {
        bleManager.startScan()
    }

    override fun onPause() {
        super.onPause()
        bleManager.stopScan()
    }
}
