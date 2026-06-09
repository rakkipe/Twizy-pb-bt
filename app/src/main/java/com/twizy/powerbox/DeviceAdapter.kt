package com.twizy.powerbox

import android.view.LayoutInflater
import android.view.ViewGroup
import androidx.recyclerview.widget.DiffUtil
import androidx.recyclerview.widget.ListAdapter
import androidx.recyclerview.widget.RecyclerView
import com.twizy.powerbox.databinding.ItemDeviceBinding

class DeviceAdapter(private val onClick: (ScannedDevice) -> Unit) :
    ListAdapter<ScannedDevice, DeviceAdapter.ViewHolder>(DIFF) {

    companion object {
        private val DIFF = object : DiffUtil.ItemCallback<ScannedDevice>() {
            override fun areItemsTheSame(a: ScannedDevice, b: ScannedDevice) = a.address == b.address
            override fun areContentsTheSame(a: ScannedDevice, b: ScannedDevice) = a == b
        }
    }

    inner class ViewHolder(private val binding: ItemDeviceBinding) :
        RecyclerView.ViewHolder(binding.root) {
        fun bind(device: ScannedDevice) {
            binding.textDeviceName.text = device.name
            binding.textDeviceAddress.text = device.address
            binding.root.setOnClickListener { onClick(device) }
        }
    }

    override fun onCreateViewHolder(parent: ViewGroup, viewType: Int): ViewHolder {
        val binding = ItemDeviceBinding.inflate(LayoutInflater.from(parent.context), parent, false)
        return ViewHolder(binding)
    }

    override fun onBindViewHolder(holder: ViewHolder, position: Int) {
        holder.bind(getItem(position))
    }
}
