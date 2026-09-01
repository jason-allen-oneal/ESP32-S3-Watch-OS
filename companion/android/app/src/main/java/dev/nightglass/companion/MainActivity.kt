package dev.nightglass.companion

import android.Manifest
import android.content.*
import android.os.Build
import android.os.Bundle
import android.net.Uri
import android.provider.Settings
import android.text.InputType
import android.view.ViewGroup
import android.widget.*
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.core.content.ContextCompat
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning
import dev.nightglass.companion.ble.NightglassConnectionService
import dev.nightglass.companion.protocol.NightglassProtocol
import dev.nightglass.companion.weather.PhoneWeatherProxy
import dev.nightglass.companion.update.OtaPackageLoader
import dev.nightglass.companion.voice.OpenClawVoiceGateway

class MainActivity : AppCompatActivity() {
    private val permissionRequest = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { grants -> if (grants.values.all { it }) connect() }
    private var resetReceiverRegistered = false
    private var otaReceiverRegistered = false
    private lateinit var otaStatus: TextView
    private val packagePicker = registerForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) inspectAndConfirmPackage(uris)
    }
    private val resetResultReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != NightglassConnectionService.ACTION_FORGET_RESULT) return
            val detail = intent.getStringExtra(
                NightglassConnectionService.EXTRA_FORGET_DETAIL)
                ?: if (intent.getBooleanExtra(
                        NightglassConnectionService.EXTRA_FORGET_SUCCESS, false))
                    "Watch authorization reset acknowledged" else
                    "Watch authorization reset failed"
            Toast.makeText(this@MainActivity, detail, Toast.LENGTH_LONG).show()
        }
    }
    private val otaProgressReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != NightglassConnectionService.ACTION_OTA_PROGRESS) return
            val detail = intent.getStringExtra(NightglassConnectionService.EXTRA_OTA_DETAIL)
                ?: "Update state unavailable"
            val percent = intent.getIntExtra(NightglassConnectionService.EXTRA_OTA_PERCENT, 0)
            otaStatus.text = if (intent.getBooleanExtra(
                    NightglassConnectionService.EXTRA_OTA_ACTIVE, false))
                "$detail — $percent%" else detail
        }
    }
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val pad = (20 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(pad, pad, pad, pad) }
        root.addView(TextView(this).apply { text = "Nightglass companion"; textSize = 26f })
        root.addView(TextView(this).apply { text = "Bluetooth pairing uses Android's system bond flow. Notification text remains in memory and is sent only over the encrypted BLE link." })
        root.addView(Button(this).apply { text = "Connect / pair Nightglass"; setOnClickListener { requestAndConnect() } })
        root.addView(Button(this).apply { text = "Grant notification access"; setOnClickListener { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) } })
        root.addView(Button(this).apply { text = "Disconnect"; setOnClickListener { startService(Intent(this@MainActivity, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_DISCONNECT)) } })
        root.addView(Button(this).apply {
            text = "Reset and re-pair watch…"
            setOnClickListener { confirmResetPinnedWatch() }
        })
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
        root.addView(TextView(this).apply {
            text = "OpenClaw voice"; textSize = 20f; setPadding(0, pad, 0, 0)
        })
        root.addView(TextView(this).apply {
            text = "Scan an OpenClaw voice-node setup QR. The constrained identity is encrypted by Android Keystore; audio is held only in memory and is never recorded by the phone microphone."
        })
        root.addView(Button(this).apply {
            text = "Scan OpenClaw voice setup QR"
            setOnClickListener { scanOpenClawVoiceSetup() }
        })
        root.addView(Button(this).apply {
            text = "Clear OpenClaw voice authorization"
            setOnClickListener {
                android.app.AlertDialog.Builder(this@MainActivity)
                    .setTitle("Clear OpenClaw voice authorization?")
                    .setMessage("Nightglass will stop sending voice turns until a new constrained setup code is entered. Revoke the old device in OpenClaw as well.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Clear") { _, _ ->
                        OpenClawVoiceGateway(this@MainActivity) { detail ->
                            Toast.makeText(this@MainActivity, detail, Toast.LENGTH_LONG).show()
                        }.also { it.clear(); it.close() }
                    }.show()
            }
        })
        root.addView(TextView(this).apply {
            text = "Signed watch update"; textSize = 20f; setPadding(0, pad, 0, 0)
        })
        root.addView(TextView(this).apply {
            text = "Select firmware.bin, manifest.json, manifest.payload, and manifest.sig from the Nightglass release packager. The phone validates the package before confirmation; the watch independently verifies its signature and writes only the inactive OTA slot."
        })
        otaStatus = TextView(this).apply { text = "No update selected" }
        root.addView(otaStatus)
        root.addView(Button(this).apply {
            text = "Select signed update package…"
            setOnClickListener { packagePicker.launch(arrayOf("*/*")) }
        })
        root.addView(Button(this).apply {
            text = "Abort update"
            setOnClickListener {
                ContextCompat.startForegroundService(this@MainActivity,
                    Intent(this@MainActivity, NightglassConnectionService::class.java)
                        .setAction(NightglassConnectionService.ACTION_CANCEL_OTA))
            }
        })
        setContentView(ScrollView(this).apply { addView(root, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)) })
    }
    private fun field(hintText: String) = EditText(this).apply { hint = hintText; setSingleLine(true) }
    private fun scanOpenClawVoiceSetup() {
        val options = GmsBarcodeScannerOptions.Builder()
            .setBarcodeFormats(Barcode.FORMAT_QR_CODE)
            .enableAutoZoom()
            .build()
        GmsBarcodeScanning.getClient(this, options).startScan()
            .addOnSuccessListener { barcode ->
                val setupCode = barcode.rawValue
                if (setupCode.isNullOrBlank()) {
                    Toast.makeText(this, "QR code did not contain OpenClaw setup data", Toast.LENGTH_LONG).show()
                    return@addOnSuccessListener
                }
                val bridge = OpenClawVoiceGateway(this) { detail ->
                    runOnUiThread { Toast.makeText(this, detail, Toast.LENGTH_LONG).show() }
                }
                try {
                    bridge.provision(setupCode)
                } catch (_: Throwable) {
                    Toast.makeText(this, "Invalid or expired OpenClaw voice setup QR", Toast.LENGTH_LONG).show()
                } finally {
                    bridge.close()
                }
            }
            .addOnFailureListener {
                Toast.makeText(this, "Unable to scan OpenClaw setup QR", Toast.LENGTH_LONG).show()
            }
    }
    override fun onStart() {
        super.onStart()
        if (!resetReceiverRegistered) {
            ContextCompat.registerReceiver(this, resetResultReceiver,
                IntentFilter(NightglassConnectionService.ACTION_FORGET_RESULT),
                ContextCompat.RECEIVER_NOT_EXPORTED)
            resetReceiverRegistered = true
        }
        if (!otaReceiverRegistered) {
            ContextCompat.registerReceiver(this, otaProgressReceiver,
                IntentFilter(NightglassConnectionService.ACTION_OTA_PROGRESS),
                ContextCompat.RECEIVER_NOT_EXPORTED)
            otaReceiverRegistered = true
        }
    }
    override fun onStop() {
        if (resetReceiverRegistered) {
            unregisterReceiver(resetResultReceiver)
            resetReceiverRegistered = false
        }
        if (otaReceiverRegistered) {
            unregisterReceiver(otaProgressReceiver)
            otaReceiverRegistered = false
        }
        super.onStop()
    }
    private fun confirmResetPinnedWatch() {
        android.app.AlertDialog.Builder(this)
            .setTitle("Reset companion authorization?")
            .setMessage("This requires the currently authorized watch. Nightglass will clear its phone allowlist first; Android clears its local pin only after the watch acknowledges the reset. You must then remove the Bluetooth bond and pair again.")
            .setNegativeButton("Cancel", null)
            .setPositiveButton("Reset and re-pair") { _, _ ->
                ContextCompat.startForegroundService(this,
                    Intent(this, NightglassConnectionService::class.java)
                        .setAction(NightglassConnectionService.ACTION_FORGET_PIN))
            }
            .show()
    }
    private fun requestAndConnect() {
        val permissions = mutableListOf<String>()
        if (Build.VERSION.SDK_INT >= 31) permissions += listOf(Manifest.permission.BLUETOOTH_SCAN, Manifest.permission.BLUETOOTH_CONNECT)
        else permissions += Manifest.permission.ACCESS_FINE_LOCATION
        if (Build.VERSION.SDK_INT >= 33) permissions += Manifest.permission.POST_NOTIFICATIONS
        permissions += listOf(Manifest.permission.READ_CALENDAR,
            Manifest.permission.READ_PHONE_STATE, Manifest.permission.ANSWER_PHONE_CALLS)
        permissionRequest.launch(permissions.toTypedArray())
    }
    private fun inspectAndConfirmPackage(uris: List<Uri>) {
        otaStatus.text = "Validating selected package…"
        Thread {
            val result = runCatching { OtaPackageLoader.load(contentResolver, uris) }
            runOnUiThread {
                val pkg = result.getOrNull()
                if (pkg == null) {
                    otaStatus.text = "Package rejected: ${result.exceptionOrNull()?.message ?: "invalid package"}"
                    return@runOnUiThread
                }
                android.app.AlertDialog.Builder(this)
                    .setTitle("Install signed Nightglass update?")
                    .setMessage("Version: ${pkg.manifest.appVersion}\nSize: ${pkg.manifest.imageSize} bytes\n\nKeep the phone and watch nearby until validation reaches 100%.")
                    .setNegativeButton("Cancel", null)
                    .setPositiveButton("Transfer update") { _, _ ->
                        uris.forEach { uri -> runCatching {
                            contentResolver.takePersistableUriPermission(uri,
                                Intent.FLAG_GRANT_READ_URI_PERMISSION)
                        } }
                        ContextCompat.startForegroundService(this,
                            Intent(this, NightglassConnectionService::class.java)
                                .setAction(NightglassConnectionService.ACTION_START_OTA)
                                .putStringArrayListExtra(
                                    NightglassConnectionService.EXTRA_OTA_URIS,
                                    ArrayList(uris.map(Uri::toString))))
                    }
                    .show()
            }
        }.start()
    }
    private fun connect() { ContextCompat.startForegroundService(this, Intent(this, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_CONNECT)) }
}
