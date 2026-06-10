package com.twizy.powerbox

import android.os.Bundle
import android.view.MenuItem
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.twizy.powerbox.databinding.ActivityDashboardBinding
import kotlinx.coroutines.launch
import org.json.JSONObject

class DashboardActivity : AppCompatActivity() {

    companion object {
        const val EXTRA_ADDRESS = "extra_device_address"
        const val EXTRA_NAME = "extra_device_name"
    }

    private lateinit var binding: ActivityDashboardBinding
    private lateinit var bleManager: BleManager

    private var relay1State = false
    private var relay2State = false
    private var isUserInteracting = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityDashboardBinding.inflate(layoutInflater)
        setContentView(binding.root)

        val address = intent.getStringExtra(EXTRA_ADDRESS) ?: run {
            finish()
            return
        }
        val name = intent.getStringExtra(EXTRA_NAME) ?: address

        setSupportActionBar(binding.toolbar)
        supportActionBar?.setDisplayHomeAsUpEnabled(true)
        supportActionBar?.title = name

        bleManager = BleManager(applicationContext)

        setupRelayToggles()
        observeConnectionState()
        observePowerBoxData()

        bleManager.connect(address)
    }

    private fun setupRelayToggles() {
        binding.switchRelay1.setOnCheckedChangeListener { _, isChecked ->
            if (isUserInteracting) {
                relay1State = isChecked
                sendRelayCommand("r1", isChecked)
            }
        }
        binding.switchRelay2.setOnCheckedChangeListener { _, isChecked ->
            if (isUserInteracting) {
                relay2State = isChecked
                sendRelayCommand("r2", isChecked)
            }
        }
    }

    private fun sendRelayCommand(relay: String, value: Boolean) {
        val cmd = JSONObject().apply {
            put("cmd", relay)
            put("val", value)
        }.toString()
        bleManager.sendCommand(cmd)
    }

    private fun observeConnectionState() {
        lifecycleScope.launch {
            bleManager.connectionState.collect { state ->
                val statusText = when (state) {
                    ConnectionState.CONNECTING -> getString(R.string.status_connecting)
                    ConnectionState.CONNECTED -> getString(R.string.status_connected)
                    ConnectionState.SCANNING -> getString(R.string.status_scanning)
                    ConnectionState.DISCONNECTED -> getString(R.string.status_disconnected)
                }
                supportActionBar?.subtitle = statusText

                if (state == ConnectionState.DISCONNECTED) {
                    binding.switchRelay1.isEnabled = false
                    binding.switchRelay2.isEnabled = false
                } else if (state == ConnectionState.CONNECTED) {
                    binding.switchRelay1.isEnabled = true
                    binding.switchRelay2.isEnabled = true
                }
            }
        }
    }

    private fun observePowerBoxData() {
        lifecycleScope.launch {
            bleManager.powerBoxData.collect { data ->
                binding.textVoltageValue.text = getString(R.string.format_voltage, data.voltage)
                binding.textCurrentValue.text = getString(R.string.format_current, data.current)
                binding.textPowerValue.text = getString(R.string.format_power, data.power)
                binding.textSocValue.text = getString(R.string.format_soc, data.soc)
                binding.progressSoc.progress = data.soc

                isUserInteracting = false
                if (data.relay1 != relay1State) {
                    relay1State = data.relay1
                    binding.switchRelay1.isChecked = data.relay1
                }
                if (data.relay2 != relay2State) {
                    relay2State = data.relay2
                    binding.switchRelay2.isChecked = data.relay2
                }
                isUserInteracting = true
            }
        }
    }

    override fun onOptionsItemSelected(item: MenuItem): Boolean {
        if (item.itemId == android.R.id.home) {
            onBackPressedDispatcher.onBackPressed()
            return true
        }
        return super.onOptionsItemSelected(item)
    }

    override fun onDestroy() {
        super.onDestroy()
        bleManager.disconnect()
    }
}
