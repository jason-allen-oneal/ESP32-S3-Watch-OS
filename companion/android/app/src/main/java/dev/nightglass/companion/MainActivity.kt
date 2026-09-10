package dev.nightglass.companion

import android.Manifest
import android.content.*
import android.graphics.Color
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
    private var voiceConversationReceiverRegistered = false
    private var statusReceiverRegistered = false
    private lateinit var otaStatus: TextView
    private lateinit var linkStatus: TextView
    private lateinit var linkStatusDetail: TextView
    private val packagePicker = registerForActivityResult(
        ActivityResultContracts.OpenMultipleDocuments()) { uris ->
        if (uris.isNotEmpty()) inspectAndStartPackage(uris)
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
    private val voiceConversationReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action !=
                NightglassConnectionService.ACTION_OPENCLAW_CONVERSATION_RESULT) return
            val detail = intent.getStringExtra(
                NightglassConnectionService.EXTRA_OPENCLAW_CONVERSATION_DETAIL)
                ?: "OpenClaw conversation state unavailable"
            Toast.makeText(this@MainActivity, detail, Toast.LENGTH_LONG).show()
        }
    }
    private val statusReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != NightglassConnectionService.ACTION_STATUS) return
            renderLinkStatus(
                intent.getStringExtra(NightglassConnectionService.EXTRA_STATUS_TEXT)
                    ?: "Status unavailable",
                intent.getStringExtra(NightglassConnectionService.EXTRA_STATUS_CLASS)
                    ?: "updated",
                intent.getLongExtra(NightglassConnectionService.EXTRA_STATUS_UPDATED_AT, 0L),
            )
        }
    }
    override fun onCreate(state: Bundle?) {
        super.onCreate(state)
        val pad = (20 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(pad, pad, pad, pad) }
        root.addView(TextView(this).apply { text = "Nightglass companion"; textSize = 26f })
        root.addView(Button(this).apply {
            text = "Watch control center"
            setOnClickListener { startActivity(Intent(this@MainActivity,
                dev.nightglass.companion.premium.ControlCenterActivity::class.java)) }
        })
        root.addView(TextView(this).apply { text = "Bluetooth pairing uses Android's system bond flow. Notification text remains in memory and is sent only over the encrypted BLE link." })
        root.addView(TextView(this).apply {
            text = "Watch link"; textSize = 20f; setPadding(0, pad, 0, 0)
        })
        linkStatus = TextView(this).apply {
            text = "WAITING FOR NIGHTGLASS"; textSize = 18f
            setPadding(0, 8, 0, 0)
        }
        root.addView(linkStatus)
        linkStatusDetail = TextView(this).apply {
            text = "Tap Connect / pair Nightglass to begin"
            setTextColor(Color.DKGRAY)
        }
        root.addView(linkStatusDetail)
        root.addView(Button(this).apply {
            text = "Connect / pair Nightglass"
            setOnClickListener { requestAndConnect() }
        })
        root.addView(Button(this).apply {
            text = "Reconnect now"
            setOnClickListener {
                ContextCompat.startForegroundService(
                    this@MainActivity,
                    Intent(this@MainActivity, NightglassConnectionService::class.java)
                        .setAction(NightglassConnectionService.ACTION_CONNECT),
                )
            }
        })
        root.addView(Button(this).apply { text = "Grant notification access"; setOnClickListener { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) } })
        root.addView(Button(this).apply { text = "Disconnect"; setOnClickListener { startService(Intent(this@MainActivity, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_DISCONNECT)) } })
        root.addView(Button(this).apply {
            text = "Reset and re-pair watch…"
            setOnClickListener { confirmResetPinnedWatch() }
        })
        root.addView(TextView(this).apply {
            text = "Direct app bridges"; textSize = 20f; setPadding(0, pad, 0, 0)
        })
        root.addView(TextView(this).apply {
            text = "The watch uses the existing encrypted link for Spotify playback and Discord notification actions. These buttons open the installed phone apps without storing provider credentials in Nightglass."
        })
        root.addView(Button(this).apply {
            text = "Open Spotify"
            setOnClickListener { launchPackage("com.spotify.music") }
        })
        root.addView(Button(this).apply {
            text = "Open Discord"
            setOnClickListener { launchPackage("com.discord") }
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
            text = "Scan an OpenClaw --voice-node setup QR. The first voice turn requests the stock approval-bound operator.write upgrade. The constrained identity is encrypted by Android Keystore; audio is held only in memory and is never recorded by the phone microphone."
        })
        root.addView(Button(this).apply {
            text = "Scan OpenClaw voice setup QR"
            setOnClickListener { scanOpenClawVoiceSetup() }
        })
        root.addView(Button(this).apply {
            text = "Start new watch conversation"
            setOnClickListener {
                ContextCompat.startForegroundService(this@MainActivity,
                    Intent(this@MainActivity, NightglassConnectionService::class.java)
                        .setAction(
                            NightglassConnectionService.ACTION_NEW_OPENCLAW_CONVERSATION))
            }
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
            text = "Select firmware.bin, manifest.json, manifest.payload, and manifest.sig from the Nightglass release packager. The phone validates the package; the watch independently verifies its signature and writes only the inactive OTA slot."
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
        NightglassConnectionService.lastStatus(this)?.let { status ->
            renderLinkStatus(status.text, status.statusClass, status.updatedAtMs)
        }
    }
    private fun field(hintText: String) = EditText(this).apply { hint = hintText; setSingleLine(true) }
    private fun launchPackage(packageName: String) {
        val intent = packageManager.getLaunchIntentForPackage(packageName)
            ?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        if (intent == null) {
            Toast.makeText(this, "App is not installed", Toast.LENGTH_SHORT).show()
            return
        }
        runCatching { startActivity(intent) }
            .onFailure { Toast.makeText(this, "Unable to open app", Toast.LENGTH_SHORT).show() }
    }
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
                    android.util.Log.i("NightglassLink",
                        "OpenClaw voice provisioning: stored")
                    ContextCompat.startForegroundService(this,
                        Intent(this, NightglassConnectionService::class.java)
                            .setAction(NightglassConnectionService.ACTION_REFRESH_OPENCLAW_HEALTH))
                } catch (error: Throwable) {
                    android.util.Log.w("NightglassLink",
                        "OpenClaw voice provisioning: rejected class=${error.javaClass.simpleName}")
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
        if (!voiceConversationReceiverRegistered) {
            ContextCompat.registerReceiver(this, voiceConversationReceiver,
                IntentFilter(
                    NightglassConnectionService.ACTION_OPENCLAW_CONVERSATION_RESULT),
                ContextCompat.RECEIVER_NOT_EXPORTED)
            voiceConversationReceiverRegistered = true
        }
        if (!statusReceiverRegistered) {
            ContextCompat.registerReceiver(this, statusReceiver,
                IntentFilter(NightglassConnectionService.ACTION_STATUS),
                ContextCompat.RECEIVER_NOT_EXPORTED)
            statusReceiverRegistered = true
        }
        NightglassConnectionService.lastStatus(this)?.let { status ->
            renderLinkStatus(status.text, status.statusClass, status.updatedAtMs)
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
        if (voiceConversationReceiverRegistered) {
            unregisterReceiver(voiceConversationReceiver)
            voiceConversationReceiverRegistered = false
        }
        if (statusReceiverRegistered) {
            unregisterReceiver(statusReceiver)
            statusReceiverRegistered = false
        }
        super.onStop()
    }
    private fun renderLinkStatus(text: String, statusClass: String, updatedAtMs: Long) {
        if (!::linkStatus.isInitialized || !::linkStatusDetail.isInitialized) return
        val ageMs = if (updatedAtMs > 0L)
            (System.currentTimeMillis() - updatedAtMs).coerceAtLeast(0L) else Long.MAX_VALUE
        val stale = ageMs > 2 * 60_000L && statusClass == "connected"
        linkStatus.text = if (stale) "LAST KNOWN — $text" else text
        linkStatus.setTextColor(when {
            stale -> Color.rgb(180, 110, 0)
            statusClass == "connected" -> Color.rgb(0, 125, 70)
            statusClass in setOf("connecting", "securing", "pairing") -> Color.rgb(30, 90, 170)
            statusClass in setOf("disconnected", "not_found", "authorization", "bluetooth") -> Color.rgb(170, 35, 45)
            else -> Color.DKGRAY
        })
        val age = if (ageMs == Long.MAX_VALUE) ""
        else " · updated ${ageMs / 1000L}s ago"
        linkStatusDetail.text = when {
            stale -> "The link may have gone stale; tap Reconnect now$age"
            statusClass == "connected" -> "Secure, encrypted, pinned link ready$age"
            statusClass == "pairing" -> "Accept Android's pairing prompt on the phone$age"
            statusClass == "authorization" -> "The watch did not authorize this phone; re-pair if needed$age"
            statusClass in setOf("disconnected", "not_found", "bluetooth") -> "The companion will retry automatically$age"
            statusClass in setOf("connecting", "securing") -> "Negotiating the secure watch link$age"
            else -> "Connection state is being refreshed$age"
        }
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
    private fun inspectAndStartPackage(uris: List<Uri>) {
        otaStatus.text = "Validating selected package…"
        Thread {
            val result = runCatching { OtaPackageLoader.load(contentResolver, uris) }
            runOnUiThread {
                val pkg = result.getOrNull()
                if (pkg == null) {
                    otaStatus.text = "Package rejected: ${result.exceptionOrNull()?.message ?: "invalid package"}"
                    return@runOnUiThread
                }
                // Selecting a package is the owner's explicit install request. The
                // phone still validates all four signed-package files before the
                // authenticated transport starts; no redundant second prompt is
                // needed here.
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
        }.start()
    }
    private fun connect() { ContextCompat.startForegroundService(this, Intent(this, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_CONNECT)) }
}
