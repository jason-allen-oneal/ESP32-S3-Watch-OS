package dev.nightglass.companion.phone

import android.Manifest
import android.annotation.SuppressLint
import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.BatteryManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.CalendarContract
import android.provider.MediaStore
import android.telecom.TelecomManager
import android.telephony.PhoneStateListener
import android.telephony.TelephonyManager
import androidx.core.content.ContextCompat
import dev.nightglass.companion.ble.NightglassConnectionService
import dev.nightglass.companion.protocol.NightglassProtocol
import java.security.SecureRandom
import java.util.concurrent.Executor

/** Bounded adapter for privileged phone state. No contact number or calendar description is relayed. */
class PhoneIntegrationManager(
    private val context: Context,
    private val executor: Executor,
) {
    private val handler = Handler(Looper.getMainLooper())
    private var finder: Ringtone? = null
    private val stopFinder = Runnable { stopPhoneRing() }
    private var callState = TelephonyManager.CALL_STATE_IDLE
    private val random = SecureRandom()
    private var callSessionId = 0u
    private var callGeneration = 0
    private val callCommandWindow = NightglassProtocol.CallCommandWindow()
    private data class CallSignature(val ringing: Boolean, val active: Boolean,
                                     val muted: Boolean)
    private var lastCallSignature: CallSignature? = null
    private val phoneListener = object : PhoneStateListener() {
        @Deprecated("PhoneStateListener compatibility")
        override fun onCallStateChanged(state: Int, phoneNumber: String?) {
            callState = state
            relayCallState()
        }
    }
    private val batteryReceiver = object : BroadcastReceiver() {
        override fun onReceive(receiverContext: Context?, intent: Intent?) {
            if (intent?.action == Intent.ACTION_BATTERY_CHANGED) relayBattery(intent)
        }
    }

    fun start() {
        ContextCompat.registerReceiver(context, batteryReceiver,
            IntentFilter(Intent.ACTION_BATTERY_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
        if (has(Manifest.permission.READ_PHONE_STATE)) {
            @Suppress("DEPRECATION")
            context.getSystemService(TelephonyManager::class.java)
                .listen(phoneListener, PhoneStateListener.LISTEN_CALL_STATE)
        }
        refreshAll()
    }

    fun stop() {
        runCatching { context.unregisterReceiver(batteryReceiver) }
        @Suppress("DEPRECATION")
        runCatching { context.getSystemService(TelephonyManager::class.java)
            .listen(phoneListener, PhoneStateListener.LISTEN_NONE) }
        handler.removeCallbacks(stopFinder)
        stopPhoneRing()
    }

    fun refreshAll() {
        context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
            ?.let(::relayBattery)
        relayCallState()
        refreshAgenda()
    }

    fun refreshAgenda() {
        if (!has(Manifest.permission.READ_CALENDAR)) {
            NightglassConnectionService.send(context, NightglassProtocol.agenda(emptyList()))
            return
        }
        executor.execute {
            val now = System.currentTimeMillis()
            val end = now + 24L * 60L * 60L * 1000L
            val events = ArrayList<NightglassProtocol.AgendaEvent>(3)
            val uri = CalendarContract.Instances.CONTENT_URI.buildUpon()
                .appendPath(now.toString()).appendPath(end.toString()).build()
            val projection = arrayOf(
                CalendarContract.Instances.BEGIN, CalendarContract.Instances.END,
                CalendarContract.Instances.TITLE, CalendarContract.Instances.EVENT_LOCATION,
                CalendarContract.Instances.ALL_DAY,
            )
            runCatching {
                context.contentResolver.query(uri, projection, null, null,
                    CalendarContract.Instances.BEGIN + " ASC")?.use { cursor ->
                    while (cursor.moveToNext() && events.size < 3) {
                        val start = cursor.getLong(0) / 1000L
                        val finish = maxOf(start, cursor.getLong(1) / 1000L)
                        events += NightglassProtocol.AgendaEvent(start, finish,
                            cursor.getString(2).orEmpty(), cursor.getString(3).orEmpty(),
                            cursor.getInt(4) != 0)
                    }
                }
            }
            NightglassConnectionService.send(context, NightglassProtocol.agenda(events))
        }
    }

    @SuppressLint("MissingPermission")
    fun handleCall(command: Int, sessionId: UInt, generation: Int,
                   sequence: Int): Boolean {
        if (command == 2 && Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return false
        val action = NightglassProtocol.WatchAction.Call(
            sequence, command, sessionId, generation)
        if (!has(Manifest.permission.ANSWER_PHONE_CALLS) ||
            !callCommandWindow.accept(action)) {
            return false
        }
        // Consume before performing the side effect. A failed platform call is
        // still not safe to repeat from a duplicated GATT notification.
        val telecom = context.getSystemService(TelecomManager::class.java)
        return when (command) {
            1 -> if (callState == TelephonyManager.CALL_STATE_RINGING) runCatching {
                @Suppress("DEPRECATION") telecom.acceptRingingCall()
                true
            }.getOrDefault(false) else false
            2 -> if (callState != TelephonyManager.CALL_STATE_IDLE) runCatching {
                @Suppress("DEPRECATION") telecom.endCall()
                true
            }.getOrDefault(false) else false
            3, 4 -> if (callState != TelephonyManager.CALL_STATE_IDLE) {
                val audio = context.getSystemService(AudioManager::class.java)
                audio.isMicrophoneMute = command == 3
                relayCallState()
                true
            } else false
            else -> false
        }
    }

    fun handlePhone(command: Int) {
        when (command) {
            1 -> startPhoneRing()
            2 -> stopPhoneRing()
            3 -> launchCamera()
            4 -> launchPackage(SPOTIFY_PACKAGE)
            5 -> launchPackage(DISCORD_PACKAGE)
        }
    }

    private fun relayBattery(intent: Intent) {
        val level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
        val scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
        if (level < 0 || scale <= 0) return
        val percent = (level * 100 / scale).coerceIn(0, 100)
        val status = intent.getIntExtra(BatteryManager.EXTRA_STATUS, -1)
        val charging = status == BatteryManager.BATTERY_STATUS_CHARGING ||
            status == BatteryManager.BATTERY_STATUS_FULL
        val powerSave = context.getSystemService(PowerManager::class.java).isPowerSaveMode
        NightglassConnectionService.send(context,
            NightglassProtocol.phoneBattery(percent, charging, powerSave))
    }

    private fun relayCallState() {
        val audio = context.getSystemService(AudioManager::class.java)
        val ringing = callState == TelephonyManager.CALL_STATE_RINGING
        val active = callState == TelephonyManager.CALL_STATE_OFFHOOK
        val signature = CallSignature(ringing, active, audio.isMicrophoneMute)
        if (!ringing && !active) {
            callSessionId = 0u
            callGeneration = 0
        } else if (callSessionId == 0u) {
            do callSessionId = random.nextInt().toUInt() while (callSessionId == 0u)
            callGeneration = 1
        } else if (lastCallSignature != signature) {
            callGeneration = (callGeneration + 1) and 0xffff
            if (callGeneration == 0) callGeneration = 1
        }
        callCommandWindow.updateSession(callSessionId, callGeneration)
        lastCallSignature = signature
        NightglassConnectionService.send(context, NightglassProtocol.callState(
            ringing, active, audio.isMicrophoneMute, ringing,
            callState != TelephonyManager.CALL_STATE_IDLE,
            callSessionId, callGeneration,
            when { ringing -> "Incoming call"; active -> "Call in progress"; else -> "" }))
    }

    private fun startPhoneRing() {
        stopPhoneRing()
        finder = runCatching {
            val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_ALARM)
            RingtoneManager.getRingtone(context, uri)?.also { it.play() }
        }.getOrNull()
        handler.postDelayed(stopFinder, 30_000L)
    }

    private fun stopPhoneRing() {
        handler.removeCallbacks(stopFinder)
        runCatching { finder?.stop() }
        finder = null
    }

    private fun launchCamera() {
        val intent = Intent(MediaStore.INTENT_ACTION_STILL_IMAGE_CAMERA)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        runCatching { context.startActivity(intent) }
    }

    private fun launchPackage(packageName: String) {
        val intent = context.packageManager.getLaunchIntentForPackage(packageName)
            ?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            ?: return
        runCatching { context.startActivity(intent) }
    }

    private fun has(permission: String) = ContextCompat.checkSelfPermission(
        context, permission) == PackageManager.PERMISSION_GRANTED

    private companion object {
        const val SPOTIFY_PACKAGE = "com.spotify.music"
        const val DISCORD_PACKAGE = "com.discord"
    }
}
