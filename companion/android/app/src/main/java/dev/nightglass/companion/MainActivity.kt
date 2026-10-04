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
import android.view.View
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
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
import dev.nightglass.companion.notifications.NightglassNotificationListener

class MainActivity : AppCompatActivity() {
    private val permissionRequest = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { grants -> if (grants.filterKeys { it != Manifest.permission.POST_NOTIFICATIONS }.values.all { it }) connect() else Toast.makeText(this, "Bluetooth permission is needed to connect your watch. Try again when ready.", Toast.LENGTH_LONG).show() }
    private var resetReceiverRegistered = false
    private var otaReceiverRegistered = false
    private var voiceConversationReceiverRegistered = false
    private var statusReceiverRegistered = false
    private var selectedPage = 0
    private lateinit var clawAction: Button
    private lateinit var clawStatus: TextView
    private lateinit var clawDetail: TextView
    private lateinit var watchAction: Button
    private val uiHandler = Handler(Looper.getMainLooper())
    private val statusTick = object : Runnable {
        override fun run() {
            NightglassConnectionService.lastStatus(this@MainActivity)?.let {
                renderLinkStatus(it.text, it.statusClass, it.updatedAtMs)
            }
            renderOpenClawStatus()
            uiHandler.postDelayed(this, 5000)
        }
    }
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
        val pad = dp(20)
        val shell = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(12, 15, 22))
            setPadding(0, dp(12), 0, 0)
        }
        val pages = listOf("Home", "Watch", "Settings").map { name ->
            LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL; setPadding(pad, pad, pad, pad)
                addView(label(name, 30f))
            }
        }
        val home = pages[0]
        home.addView(label("Your watch, at a glance", 15f))
        var root = card(home)
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
            setTextColor(Color.LTGRAY)
        }
        root.addView(linkStatusDetail)
        watchAction = Button(this).apply {
            text = "Connect watch"
            setOnClickListener { requestAndConnect() }
        }
        root.addView(watchAction)
        val watch = card(pages[1])
        watch.addView(label("Make it yours", 22f))
        watch.addView(label("Choose your face, appearance, and watch controls.", 15f))
        watch.addView(Button(this).apply {
            text = "Customize watch"
            setOnClickListener { startActivity(Intent(this@MainActivity,
                dev.nightglass.companion.premium.ControlCenterActivity::class.java)) }
        })
        root = card(pages[2])
        root.addView(label("Notifications & phone features", 20f))
        root.addView(label("Enable only the features you want on your watch.", 15f))
        root.addView(Button(this).apply {
            text = "Enable calendar and call controls"
            setOnClickListener { featurePermissions.launch(arrayOf(Manifest.permission.READ_CALENDAR,
                Manifest.permission.READ_PHONE_STATE, Manifest.permission.ANSWER_PHONE_CALLS)) }
        })
        root.addView(Button(this).apply { text = "Enable watch notifications"; setOnClickListener { startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)) } })
        root.addView(CheckBox(this).apply {
            text = "Notification sounds"
            isChecked = NightglassNotificationListener.notificationSoundsEnabled(this@MainActivity)
            setOnCheckedChangeListener { _, enabled ->
                NightglassNotificationListener.setNotificationSoundsEnabled(this@MainActivity, enabled)
            }
        })
        root.addView(TextView(this).apply {
            text = "Connected apps"; textSize = 20f; setPadding(0, pad, 0, 0)
        })
        root.addView(TextView(this).apply {
            text = "Open Spotify for music or Discord for messages. Notification access lets supported controls appear on your watch."
        })
        root.addView(Button(this).apply {
            text = "Open Spotify"
            setOnClickListener { launchPackage("com.spotify.music") }
        })
        root.addView(Button(this).apply {
            text = "Open Discord"
            setOnClickListener { launchPackage("com.discord") }
        })
        root = card(pages[2])
        root.addView(TextView(this).apply { text = "Weather"; textSize = 20f; setPadding(0, pad, 0, 0) })
        root.addView(TextView(this).apply { text = "Choose a city for your forecast. Weather uses your phone’s internet connection." })
        val ssid = field("Optional direct watch Wi-Fi name")
        val password = field("Optional direct watch Wi-Fi password").apply { inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD }
        val latitude = field("Latitude, e.g. 40.7128").apply { inputType = InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_DECIMAL or InputType.TYPE_NUMBER_FLAG_SIGNED }
        val longitude = field("Longitude, e.g. -74.0060").apply { inputType = latitude.inputType }
        val savedWeather = PhoneWeatherProxy.load(this)
        val metric = CheckBox(this).apply { text = "Use Celsius & metric units"; isChecked = savedWeather?.metric ?: false }
        val refresh = field("Refresh minutes (15–360)").apply { inputType = InputType.TYPE_CLASS_NUMBER; setText("30") }
        savedWeather?.let {
            latitude.setText((it.latitudeE6 / 1_000_000.0).toString())
            longitude.setText((it.longitudeE6 / 1_000_000.0).toString())
            refresh.setText(it.refreshMinutes.toString())
        }
        val city = field("City or postal code")
        root.addView(city)
        val weatherRoot = root
        root.addView(Button(this).apply {
            text = "Find weather location"
            setOnClickListener {
                val query = city.text.toString().trim()
                if (query.isEmpty()) {
                    city.error = "Enter a city or postal code"
                } else {
                    isEnabled = false; text = "Finding location…"
                    Thread {
                        val found = runCatching {
                            @Suppress("DEPRECATION")
                            android.location.Geocoder(this@MainActivity).getFromLocationName(query, 5)
                        }.getOrNull()
                        runOnUiThread {
                            isEnabled = true; text = "Find weather location"
                            if (!found.isNullOrEmpty()) {
                                val names = found.map { listOfNotNull(it.locality, it.adminArea, it.countryName).distinct().joinToString(", ").ifBlank { query } }
                                android.app.AlertDialog.Builder(this@MainActivity)
                                    .setTitle("Choose your weather location")
                                    .setItems(names.toTypedArray()) { _, index ->
                                        val place = found[index]
                                        latitude.setText(place.latitude.toString())
                                        longitude.setText(place.longitude.toString())
                                        city.setText(names[index])
                                        Toast.makeText(this@MainActivity, "Location selected. Tap Save and refresh weather.", Toast.LENGTH_LONG).show()
                                    }.setNegativeButton("Cancel", null).show()
                            } else city.error = "Couldn't find this place. Try another city or enter coordinates below."
                        }
                    }.start()
                }
            }
        })
        root.addView(metric)
        val weatherAdvanced = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; visibility = View.GONE }
        listOf(ssid, password, latitude, longitude, refresh).forEach(weatherAdvanced::addView)
        root.addView(Button(this).apply {
            text = "Advanced weather options"
            setOnClickListener { weatherAdvanced.visibility = if (weatherAdvanced.visibility == View.GONE) View.VISIBLE else View.GONE }
        })
        weatherRoot.addView(weatherAdvanced)
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
            } catch (_: Exception) {
                Toast.makeText(this@MainActivity, "Choose a weather location first. In advanced options, use valid coordinates and a refresh interval of 15–360 minutes.", Toast.LENGTH_LONG).show()
            }
            finally { secret.fill('\u0000'); password.text?.clear() }
        } })
        root.addView(Button(this).apply { text = "Clear watch Wi-Fi"; setOnClickListener { NightglassConnectionService.send(this@MainActivity, NightglassProtocol.clearWifi()) } })
        root = card(home)
        clawStatus = label("Not checked yet", 22f)
        clawDetail = label("Connect your watch to check OpenClaw.", 15f)
        root.addView(label("OpenClaw", 20f))
        root.addView(clawStatus)
        root.addView(clawDetail)
        clawAction = Button(this).apply {
            text = "Fix connection"
            setOnClickListener { showConnectionRecovery() }
        }
        root.addView(clawAction)
        root.addView(TextView(this).apply {
            text = "Set up voice"; textSize = 20f; setPadding(0, pad, 0, 0)
        })
        root.addView(TextView(this).apply {
            text = "1. Open OpenClaw on your computer and create a voice setup QR.\n2. Scan it here.\n3. If OpenClaw asks, approve voice access on your computer. Then speak from your watch."
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
        val advancedCard = card(pages[2])
        root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; visibility = View.GONE }
        val advancedContent = root
        advancedCard.addView(Button(this).apply {
            text = "Advanced connection options"
            setOnClickListener { advancedContent.visibility = if (advancedContent.visibility == View.GONE) View.VISIBLE else View.GONE }
        })
        advancedCard.addView(root)
        root.addView(Button(this).apply { text = "Disconnect"; setOnClickListener { startService(Intent(this@MainActivity, NightglassConnectionService::class.java).setAction(NightglassConnectionService.ACTION_DISCONNECT)) } })
        root.addView(Button(this).apply {
            text = "Reset and re-pair watch…"
            setOnClickListener { confirmResetPinnedWatch() }
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
        root = card(pages[2])
        root.addView(TextView(this).apply {
            text = "Watch updates"; textSize = 20f; setPadding(0, pad, 0, 0)
        })
        root.addView(TextView(this).apply {
            text = "Choose the four files from a signed Nightglass release package. Keep your watch nearby while the update installs."
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
        val scrolls = pages.map { page -> ScrollView(this).apply {
            isFillViewport = true; addView(page)
            visibility = View.GONE
            shell.addView(this, LinearLayout.LayoutParams(-1, 0, 1f))
        } }
        val navigation = LinearLayout(this).apply { setPadding(dp(12), dp(6), dp(12), dp(12)) }
        val tabs = mutableListOf<Button>()
        fun select(index: Int) {
            selectedPage = index
            scrolls.forEachIndexed { i, view -> view.visibility = if (i == index) View.VISIBLE else View.GONE }
            tabs.forEachIndexed { i, button -> button.setTextColor(if (i == index) Color.rgb(184, 171, 255) else Color.LTGRAY) }
        }
        listOf("Home", "Watch", "Settings").forEachIndexed { index, name ->
            val button = Button(this).apply { text = name; isAllCaps = false; setOnClickListener { select(index) } }
            tabs.add(button); navigation.addView(button, LinearLayout.LayoutParams(0, dp(56), 1f))
        }
        shell.addView(navigation)
        styleViews(shell)
        setContentView(shell)
        androidx.core.view.ViewCompat.setOnApplyWindowInsetsListener(shell) { view, insets ->
            val bars = insets.getInsets(androidx.core.view.WindowInsetsCompat.Type.systemBars())
            view.setPadding(bars.left, bars.top + dp(12), bars.right, bars.bottom)
            insets
        }
        select(state?.getInt("page", 0) ?: 0)
        renderOpenClawStatus()
        NightglassConnectionService.lastStatus(this)?.let { status ->
            renderLinkStatus(status.text, status.statusClass, status.updatedAtMs)
        }
    }
    override fun onSaveInstanceState(state: Bundle) {
        state.putInt("page", selectedPage)
        super.onSaveInstanceState(state)
    }
    private val featurePermissions = registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { grants ->
        Toast.makeText(this, if (grants.values.all { it }) "Phone features enabled" else "Some phone features remain disabled", Toast.LENGTH_LONG).show()
    }
    private fun dp(value: Int) = (value * resources.displayMetrics.density).toInt()
    private fun label(value: String, size: Float) = TextView(this).apply {
        text = value; textSize = size; setTextColor(Color.rgb(235, 237, 245))
        setPadding(0, dp(6), 0, dp(8))
    }
    private fun card(parent: LinearLayout) = LinearLayout(this).apply {
        orientation = LinearLayout.VERTICAL; setPadding(dp(18), dp(14), dp(18), dp(16))
        background = GradientDrawable().apply { setColor(Color.rgb(24, 29, 41)); cornerRadius = dp(20).toFloat() }
        parent.addView(this, LinearLayout.LayoutParams(-1, -2).apply { topMargin = dp(16) })
    }
    private fun styleViews(view: View) {
        if (view is TextView) {
            view.setTextColor(Color.rgb(230, 233, 243))
            if (view is EditText) view.setHintTextColor(Color.rgb(161, 170, 190))
            if (view is Button) {
                view.isAllCaps = false; view.minHeight = dp(52)
                view.backgroundTintList = android.content.res.ColorStateList.valueOf(Color.rgb(49, 43, 69))
                view.setTextColor(Color.rgb(204, 193, 255))
            }
        }
        if (view is ViewGroup) for (i in 0 until view.childCount) styleViews(view.getChildAt(i))
    }
    private fun showConnectionRecovery() {
        val watch = NightglassConnectionService.lastStatus(this)
        if (watch?.statusClass != "connected") {
            android.app.AlertDialog.Builder(this).setTitle("Connect your watch first")
                .setMessage("Keep the watch nearby and Bluetooth on. We’ll check OpenClaw after the watch connects.")
                .setNegativeButton("Not now", null)
                .setPositiveButton("Connect watch") { _, _ -> requestAndConnect() }.show()
            return
        }
        val configured = getSharedPreferences("nightglass_openclaw_voice", MODE_PRIVATE).contains("credential")
        if (!configured) { scanOpenClawVoiceSetup(); return }
        android.app.AlertDialog.Builder(this).setTitle("Let’s get voice working")
            .setMessage("1. Check that OpenClaw is running on your computer.\n\n2. Make sure your phone can reach that computer over your network.\n\n3. Check again. You don’t need to reset your watch.\n\nIf access was revoked or your OpenClaw address changed, scan a new setup QR.")
            .setNegativeButton("Not now", null)
            .setNeutralButton("Scan new QR") { _, _ -> scanOpenClawVoiceSetup() }
            .setPositiveButton("Check again") { _, _ ->
                clawStatus.text = "Checking…"
                clawDetail.text = "Checking the phone’s connection to OpenClaw."
                ContextCompat.startForegroundService(this,
                    Intent(this, NightglassConnectionService::class.java)
                        .setAction(NightglassConnectionService.ACTION_REFRESH_OPENCLAW_HEALTH))
            }.show()
    }
    private fun renderOpenClawStatus() {
        val prefs = getSharedPreferences("nightglass_openclaw_health", MODE_PRIVATE)
        val time = prefs.getLong("checkedAt", 0)
        val fresh = time > 0 && System.currentTimeMillis() - time < 150000
        val watch = NightglassConnectionService.lastStatus(this)
        val linked = watch?.statusClass == "connected" && System.currentTimeMillis() - watch.updatedAtMs < 120000
        val configured = getSharedPreferences("nightglass_openclaw_voice", MODE_PRIVATE).contains("credential") && (!fresh || prefs.getBoolean("configured", false))
        val healthy = linked && fresh && prefs.getBoolean("reachable", false)
        clawAction.text = when { !linked -> "Connect watch"; !configured -> "Set up OpenClaw"; healthy -> "Connection details"; else -> "Fix connection" }
        clawStatus.text = when {
            !linked -> "Waiting for watch"
            !configured -> "Setup needed"
            healthy -> "Ready to talk"
            !fresh -> "Not checked yet"
            !prefs.getBoolean("internet", false) -> "Phone is offline"
            prefs.getBoolean("fatal", false) -> "Access needs attention"
            else -> "Can't reach OpenClaw"
        }
        clawDetail.text = when {
            !linked -> "Connect your watch first. Bluetooth and OpenClaw are separate connections."
            !configured -> "Scan a setup QR below to enable voice."
            healthy -> "Speak from your watch to start a conversation."
            !fresh -> "Tap Check connection for a fresh result."
            !prefs.getBoolean("internet", false) -> "Connect your phone to Wi-Fi or mobile data, then check again."
            prefs.getBoolean("fatal", false) -> "Check device access in OpenClaw on your computer. You may need a new setup QR."
            else -> "Make sure OpenClaw is running and reachable from your phone, then check again."
        }
        clawStatus.setTextColor(if (healthy) Color.rgb(111, 220, 169) else Color.rgb(244, 194, 118))
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
        uiHandler.post(statusTick)
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
        uiHandler.removeCallbacks(statusTick)
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
        linkStatus.text = when {
            stale -> "Connection needs checking"
            statusClass == "connected" -> "Watch connected"
            statusClass == "pairing" -> "Confirm pairing on your phone"
            statusClass in setOf("connecting", "securing") -> "Connecting…"
            else -> "Watch disconnected"
        }
        watchAction.visibility = if (statusClass == "connected" && !stale) View.GONE else View.VISIBLE
        watchAction.text = if (statusClass == "authorization") "Try connecting again" else "Connect watch"
        linkStatus.setTextColor(when {
            stale -> Color.rgb(180, 110, 0)
            statusClass == "connected" -> Color.rgb(0, 125, 70)
            statusClass in setOf("connecting", "securing", "pairing") -> Color.rgb(30, 90, 170)
            statusClass in setOf("disconnected", "not_found", "authorization", "bluetooth") -> Color.rgb(170, 35, 45)
            else -> Color.LTGRAY
        })
        val age = if (ageMs == Long.MAX_VALUE) ""
        else " · updated ${ageMs / 1000L}s ago"
        linkStatusDetail.text = when {
            stale -> "The link may have gone stale; tap Connect watch$age"
            statusClass == "connected" -> "Ready for notifications and watch controls$age"
            statusClass == "pairing" -> "Accept Android's pairing prompt on the phone$age"
            statusClass == "authorization" -> "The watch did not authorize this phone. Try connecting again; reset pairing only if that fails.$age"
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
