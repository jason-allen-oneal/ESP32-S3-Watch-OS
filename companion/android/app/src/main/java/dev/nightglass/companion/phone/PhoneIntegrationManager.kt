package dev.nightglass.companion.phone

import android.Manifest
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

    fun handleCall(command: Int) {
        if (!has(Manifest.permission.ANSWER_PHONE_CALLS)) return
        val telecom = context.getSystemService(TelecomManager::class.java)
        when (command) {
            1 -> if (callState == TelephonyManager.CALL_STATE_RINGING) runCatching {
                @Suppress("DEPRECATION") telecom.acceptRingingCall()
            }
            2 -> if (callState != TelephonyManager.CALL_STATE_IDLE) runCatching {
                @Suppress("DEPRECATION") telecom.endCall()
            }
            3 -> if (callState != TelephonyManager.CALL_STATE_IDLE) {
                val audio = context.getSystemService(AudioManager::class.java)
                audio.isMicrophoneMute = !audio.isMicrophoneMute
                relayCallState()
            }
        }
    }

    fun handlePhone(command: Int) {
        when (command) {
            1 -> startPhoneRing()
            2 -> stopPhoneRing()
            3 -> launchCamera()
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
        NightglassConnectionService.send(context, NightglassProtocol.callState(
            ringing, active, audio.isMicrophoneMute, ringing,
            callState != TelephonyManager.CALL_STATE_IDLE,
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

    private fun has(permission: String) = ContextCompat.checkSelfPermission(
        context, permission) == PackageManager.PERMISSION_GRANTED
}
