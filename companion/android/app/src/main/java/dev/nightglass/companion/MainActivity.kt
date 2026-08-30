package dev.nightglass.companion

import android.Manifest
import android.content.*
import android.os.Build
import android.os.Bundle
import android.provider.Settings
import android.text.InputType
import android.view.ViewGroup
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import dev.nightglass.companion.ble.NightglassConnectionService
import dev.nightglass.companion.protocol.NightglassProtocol

class MainActivity : AppCompatActivity() {
    private val permissionRequest = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { grants -> if (grants.values.all { it }) connect() }
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val pad = (20 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(pad, pad, pad, pad) }
        root.addView(TextView(this).apply { text = "Nightglass companion"; textSize = 26f })
        root.addView(TextView(this).apply { text = "Bluetooth pairing uses Android's system bond flow. Notification text remains in memory and is sent only over the encrypted BLE link." })
        root.addView(Button(this).apply { text = "Connect / pair Nightglass"; setOnClickListener { requestAndConnect() } })
        root.addView(Button(this).apply { text = "Grant notification access"; setOnClickListener { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) } })
        root.addView(Button(this).apply { text = "Disconnect"; setOnClickListener { startService(Intent(this@MainActivity, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_DISCONNECT)) } })
        root.addView(TextView(this).apply { text = "Wi-Fi and weather location"; textSize = 20f; setPadding(0, pad, 0, 0) })
        root.addView(TextView(this).apply { text = "Credentials and fixed weather location are sent only over the bonded encrypted link. Secrets are never saved or logged." })
        val ssid = field("Wi-Fi network name")
        val password = field("Wi-Fi password").apply { inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD }
        val latitude = field("Latitude, e.g. 40.7128").apply { inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED }
        val longitude = field("Longitude, e.g. -74.0060").apply { inputType = latitude.inputType }
        val metric = CheckBox(this).apply { text = "Metric units"; isChecked = false }
        val refresh = field("Refresh minutes (15–360)").apply { inputType = InputType.TYPE_CLASS_NUMBER; setText("30") }
        listOf(ssid, password, latitude, longitude, metric, refresh).forEach(root::addView)
        root.addView(Button(this).apply { text = "Send provisioning frame"; setOnClickListener {
            val secret = password.text.toString().toCharArray()
            try {
                val lat = ((latitude.text.toString().toDouble()) * 1_000_000).toInt()
                val lon = ((longitude.text.toString().toDouble()) * 1_000_000).toInt()
                val wifiQueued = NightglassConnectionService.send(this, NightglassProtocol.provisionWifi(ssid.text.toString(), secret))
                val weatherQueued = NightglassConnectionService.send(this, NightglassProtocol.configureWeather(true, true, metric.isChecked, refresh.text.toString().toInt(), lat, lon))
                Toast.makeText(this@MainActivity, if (wifiQueued && weatherQueued) "Wi-Fi and weather settings queued" else "Unable to start Nightglass link", Toast.LENGTH_SHORT).show()
            } catch (_: Exception) { Toast.makeText(this@MainActivity, "Check network name and coordinates", Toast.LENGTH_LONG).show() }
            finally { secret.fill('\u0000'); password.text?.clear() }
        } })
        root.addView(Button(this).apply { text = "Clear watch Wi-Fi"; setOnClickListener { NightglassConnectionService.send(this@MainActivity, NightglassProtocol.clearWifi()) } })
        setContentView(ScrollView(this).apply { addView(root, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)) })
    }
    private fun field(hintText: String) = EditText(this).apply { hint = hintText; setSingleLine(true) }
    private fun requestAndConnect() {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) permissions += listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else permissions += Manifest.permission.ACCESS_FINE_LOCATION
        if (Build.VERSION.SDK_INT >= 33) permissions += Manifest.permission.POST_NOTIFICATIONS
        permissionRequest.launch(permissions.toTypedArray())
    }
    private fun connect() { ContextCompat.startForegroundService(this, Intent(this, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_CONNECT)) }
}
