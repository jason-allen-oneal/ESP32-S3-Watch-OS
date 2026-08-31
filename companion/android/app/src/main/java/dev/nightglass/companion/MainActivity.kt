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
import dev.nightglass.companion.weather.PhoneWeatherProxy

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
        root.addView(Button(this).apply { text = "Reset pinned watch"; setOnClickListener { startService(Intent(this@MainActivity, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_FORGET_PIN)); Toast.makeText(this@MainActivity, "Pinned watch cleared. Remove the old Bluetooth bond before pairing again.", Toast.LENGTH_LONG).show() } })
        root.addView(TextView(this).apply { text = "Phone weather proxy"; textSize = 20f; setPadding(0, pad, 0, 0) })
        root.addView(TextView(this).apply { text = "Weather uses whichever Internet connection this phone has (Wi-Fi or cellular). Direct watch Wi-Fi is an optional fallback; its password is never saved or logged." })
        val ssid = field("Optional direct watch Wi-Fi name")
        val password = field("Optional direct watch Wi-Fi password").apply { inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD }
        val latitude = field("Latitude, e.g. 40.7128").apply { inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED }
        val longitude = field("Longitude, e.g. -74.0060").apply { inputType = latitude.inputType }
        val metric = CheckBox(this).apply { text = "Metric units"; isChecked = false }
        val refresh = field("Refresh minutes (15–360)").apply { inputType = InputType.TYPE_CLASS_NUMBER; setText("30") }
        listOf(ssid, password, latitude, longitude, metric, refresh).forEach(root::addView)
        root.addView(Button(this).apply { text = "Save and refresh weather"; setOnClickListener {
            val secret = password.text.toString().toCharArray()
            try {
                val lat = ((latitude.text.toString().toDouble()) * 1_000_000).toInt()
                val lon = ((longitude.text.toString().toDouble()) * 1_000_000).toInt()
                val refreshMinutes = refresh.text.toString().toInt()
                val proxyConfig = PhoneWeatherProxy.Config(lat, lon, metric.isChecked, refreshMinutes)
                PhoneWeatherProxy.save(this@MainActivity, proxyConfig)
                val wifiQueued = ssid.text.isBlank() || NightglassConnectionService.send(
                    this@MainActivity, NightglassProtocol.provisionWifi(ssid.text.toString(), secret))
                val weatherQueued = NightglassConnectionService.send(this@MainActivity, NightglassProtocol.configureWeather(true, true, metric.isChecked, refreshMinutes, lat, lon))
                ContextCompat.startForegroundService(this@MainActivity,
                    Intent(this@MainActivity, NightglassConnectionService::class.java)
                        .setAction(NightglassConnectionService.ACTION_REFRESH_WEATHER))
                Toast.makeText(this@MainActivity, if (wifiQueued && weatherQueued) "Phone weather refresh queued" else "Unable to start Nightglass link", Toast.LENGTH_SHORT).show()
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
