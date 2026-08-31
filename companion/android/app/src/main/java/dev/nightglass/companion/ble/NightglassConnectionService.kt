package dev.nightglass.companion.ble

import android.Manifest
import android.app.*
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.*
import android.content.pm.PackageManager
import android.media.AudioManager
import android.net.ConnectivityManager
import android.net.Network
import android.os.*
import android.view.KeyEvent
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationCompat
import androidx.core.content.ContextCompat
import dev.nightglass.companion.protocol.NightglassProtocol
import dev.nightglass.companion.notifications.NightglassNotificationListener
import dev.nightglass.companion.weather.PhoneWeatherProxy
import java.util.ArrayDeque
import java.util.concurrent.Executors

class NightglassConnectionService : Service() {
    companion object {
        const val ACTION_CONNECT = "dev.nightglass.CONNECT"
        const val ACTION_DISCONNECT = "dev.nightglass.DISCONNECT"
        const val ACTION_WRITE = "dev.nightglass.WRITE"
        const val ACTION_REFRESH_WEATHER = "dev.nightglass.REFRESH_WEATHER"
        const val EXTRA_FRAME = "frame"
        const val CHANNEL = "nightglass_connection"
        @Volatile private var current: NightglassConnectionService? = null
        fun send(context: Context, frame: ByteArray): Boolean = runCatching {
            val intent = Intent(context, NightglassConnectionService::class.java)
                .setAction(ACTION_WRITE).putExtra(EXTRA_FRAME, frame)
            ContextCompat.startForegroundService(context, intent)
            true
        }.getOrDefault(false)
    }
    private val adapter by lazy { getSystemService(BluetoothManager::class.java).adapter }
    private var gatt: BluetoothGatt? = null
    private var scanning = false
    private var negotiatedPayload = 20
    private val writes = ArrayDeque<ByteArray>()
    private var writePending = false
    private var linkReady = false
    private val reconnectHandler = Handler(Looper.getMainLooper())
    private var reconnectAttempt = 0
    private var explicitDisconnect = false
    private val reconnect = Runnable { if (!explicitDisconnect && gatt == null) scan() }
    private val weatherExecutor = Executors.newSingleThreadExecutor()
    @Volatile private var destroyed = false
    private var weatherFetchInFlight = false
    private val weatherRefresh = Runnable { fetchPhoneWeather() }
    private val networkCallback = object : ConnectivityManager.NetworkCallback() {
        override fun onAvailable(network: Network) { scheduleWeatherRefresh(0) }
    }

    private val bondReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            if (intent?.action != BluetoothDevice.ACTION_BOND_STATE_CHANGED || !hasConnectPermissions()) return
            val device = if (Build.VERSION.SDK_INT >= 33) intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE, BluetoothDevice::class.java) else @Suppress("DEPRECATION") intent.getParcelableExtra(BluetoothDevice.EXTRA_DEVICE)
            if (device != null && device.address == gatt?.device?.address &&
                device.bondState == BluetoothDevice.BOND_BONDED) gatt?.requestMtu(247)
        }
    }
    override fun onCreate() {
        super.onCreate()
        destroyed = false
        current = this
        createChannel()
        ContextCompat.registerReceiver(this, bondReceiver,
            IntentFilter(BluetoothDevice.ACTION_BOND_STATE_CHANGED),
            ContextCompat.RECEIVER_EXPORTED)
        runCatching { getSystemService(ConnectivityManager::class.java)
            .registerDefaultNetworkCallback(networkCallback) }
    }
    override fun onDestroy() {
        destroyed = true
        explicitDisconnect = true
        reconnectHandler.removeCallbacks(reconnect)
        current = null
        unregisterReceiver(bondReceiver)
        runCatching { getSystemService(ConnectivityManager::class.java)
            .unregisterNetworkCallback(networkCallback) }
        stopScan()
        reconnectHandler.removeCallbacks(weatherRefresh)
        weatherExecutor.shutdownNow()
        closeGatt()
        super.onDestroy()
    }
    override fun onBind(intent: Intent?) = null
    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        startForeground(7, connectionNotification("Searching for Nightglass"))
        when (intent?.action) {
            ACTION_DISCONNECT -> { explicitDisconnect = true; reconnectHandler.removeCallbacks(reconnect); stopScan(); closeGatt(); stopSelf() }
            ACTION_WRITE -> {
                explicitDisconnect = false
                intent.getByteArrayExtra(EXTRA_FRAME)?.let(::write)
                if (gatt == null) scan()
            }
            ACTION_REFRESH_WEATHER -> {
                explicitDisconnect = false
                scheduleWeatherRefresh(0)
                if (gatt == null) scan()
            }
            else -> { explicitDisconnect = false; scan() }
        }
        return START_STICKY
    }

    private fun scan() {
        if (!hasConnectPermissions()) {
            update("Bluetooth permission unavailable; retrying")
            scheduleReconnect()
            return
        }
        if (!adapter.isEnabled) {
            update("Bluetooth is off; retrying")
            scheduleReconnect()
            return
        }
        if (scanning) return
        scanning = true
        // Some Samsung Bluetooth stacks fail to return custom 128-bit UUID advertisements
        // through a platform ScanFilter. Scan broadly, then strictly allowlist Nightglass.
        adapter.bluetoothLeScanner?.startScan(null, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), scanCallback)
        reconnectHandler.postDelayed({
            if (scanning) {
                stopScan()
                update("Nightglass not found; retrying")
                scheduleReconnect()
            }
        }, 15_000)
    }
    private val scanCallback = object : ScanCallback() {
        override fun onScanResult(type: Int, result: ScanResult) {
            val advertisedServices = result.scanRecord?.serviceUuids.orEmpty()
            val advertisedName = result.scanRecord?.deviceName ?: runCatching { result.device.name }.getOrNull()
            if (advertisedServices.none { it.uuid == NightglassProtocol.SERVICE } && advertisedName != "Nightglass") return
            stopScan()
            connect(result.device)
        }
        override fun onScanFailed(errorCode: Int) {
            scanning = false
            update("Bluetooth scan unavailable ($errorCode); retrying")
            scheduleReconnect()
        }
    }
    private fun connect(device: BluetoothDevice) {
        if (gatt != null) return
        resetLinkState()
        gatt = device.connectGatt(this, false, callback, BluetoothDevice.TRANSPORT_LE)
        update("Connecting")
    }
    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(client: BluetoothGatt, status: Int, state: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS && state == BluetoothProfile.STATE_CONNECTED) {
                reconnectHandler.removeCallbacks(reconnect)
                linkReady = false
                update("Connected; negotiating secure transport")
                if (!client.requestMtu(247)) client.discoverServices()
            }
            else {
                resetLinkState()
                client.close()
                if (gatt === client) gatt = null
                update("Disconnected; reconnecting")
                scheduleReconnect()
            }
        }
        override fun onMtuChanged(client: BluetoothGatt, mtu: Int, status: Int) {
            if (status == BluetoothGatt.GATT_SUCCESS && mtu >= 182) {
                negotiatedPayload = mtu - 3
                client.discoverServices()
            }
            else update("Nightglass link MTU is too small")
        }
        override fun onServicesDiscovered(client: BluetoothGatt, status: Int) {
            if (negotiatedPayload < 179) { client.requestMtu(247); return }
            if (status != BluetoothGatt.GATT_SUCCESS || client.device.bondState != BluetoothDevice.BOND_BONDED) { update("Pairing required; accept the system prompt"); return }
            val service = client.getService(NightglassProtocol.SERVICE) ?: return update("Incompatible Nightglass service")
            if (!subscribe(client, service.getCharacteristic(NightglassProtocol.WATCH_TO_PHONE)))
                update("Could not enable Nightglass notifications")
        }
        override fun onDescriptorWrite(client: BluetoothGatt, descriptor: BluetoothGattDescriptor, status: Int) {
            if (descriptor.uuid != NightglassProtocol.CCCD) return
            if (status != BluetoothGatt.GATT_SUCCESS) return update("Could not enable Nightglass notifications")
            reconnectAttempt = 0
            synchronized(writes) { linkReady = true; writeNextLocked() }
            update("Nightglass connected")
            scheduleWeatherRefresh(0)
        }
        override fun onCharacteristicWrite(client: BluetoothGatt, characteristic: BluetoothGattCharacteristic, status: Int) {
            if (status != BluetoothGatt.GATT_SUCCESS) update("Nightglass rejected a settings frame")
            synchronized(writes) { writePending = false; writeNextLocked() }
        }
        @Deprecated("API compatibility")
        override fun onCharacteristicChanged(client: BluetoothGatt, characteristic: BluetoothGattCharacteristic) { receive(characteristic.value) }
        override fun onCharacteristicChanged(client: BluetoothGatt, characteristic: BluetoothGattCharacteristic, value: ByteArray) { receive(value) }
    }
    private fun subscribe(client: BluetoothGatt, c: BluetoothGattCharacteristic?): Boolean {
        c ?: return false
        if (!client.setCharacteristicNotification(c, true)) return false
        val descriptor = c.getDescriptor(NightglassProtocol.CCCD) ?: return false
        return if (Build.VERSION.SDK_INT >= 33)
            client.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS
        else { @Suppress("DEPRECATION") descriptor.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE; @Suppress("DEPRECATION") client.writeDescriptor(descriptor) }
    }
    private fun receive(frame: ByteArray) { when (val action = NightglassProtocol.parseAction(frame)) {
        is NightglassProtocol.WatchAction.Media -> handleMedia(action.command)
        is NightglassProtocol.WatchAction.Notification -> NightglassNotificationListener.perform(action.id, action.dismiss)
        null -> Unit
    } }
    private fun handleMedia(command: Int) {
        val key = when(command) { 1 -> KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE; 2 -> KeyEvent.KEYCODE_MEDIA_NEXT; 3 -> KeyEvent.KEYCODE_MEDIA_PREVIOUS; 4 -> KeyEvent.KEYCODE_VOLUME_UP; 5 -> KeyEvent.KEYCODE_VOLUME_DOWN; else -> return }
        val audio = getSystemService(AudioManager::class.java)
        audio.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_DOWN, key)); audio.dispatchMediaKeyEvent(KeyEvent(KeyEvent.ACTION_UP, key))
    }
    private fun write(frame: ByteArray) {
        synchronized(writes) { if (writes.size >= 32) writes.removeFirst(); writes.add(frame.copyOf()); writeNextLocked() }
    }
    private fun writeNextLocked() {
        if (writePending || !linkReady) return
        val client = gatt ?: return
        val c = client.getService(NightglassProtocol.SERVICE)?.getCharacteristic(NightglassProtocol.PHONE_TO_WATCH) ?: return
        val frame = writes.peek() ?: return
        if (frame.size > negotiatedPayload) {
            writes.poll()
            update("Nightglass frame exceeds negotiated MTU")
            writeNextLocked()
            return
        }
        val started = if (Build.VERSION.SDK_INT >= 33) client.writeCharacteristic(c, frame, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS
        else { @Suppress("DEPRECATION") c.value = frame; @Suppress("DEPRECATION") client.writeCharacteristic(c) }
        writePending = started
        if (started) writes.poll()
    }
    private fun stopScan() { if (scanning && hasConnectPermissions()) adapter.bluetoothLeScanner?.stopScan(scanCallback); scanning = false }
    private fun scheduleReconnect() {
        if (explicitDisconnect) return
        reconnectHandler.removeCallbacks(reconnect)
        val delay = minOf(60_000L, 1_000L shl minOf(reconnectAttempt, 5))
        reconnectAttempt++
        reconnectHandler.postDelayed(reconnect, delay)
    }
    private fun resetLinkState() {
        synchronized(writes) { writePending = false; linkReady = false }
        negotiatedPayload = 20
    }
    private fun closeGatt() { synchronized(writes) { writes.clear(); writePending = false; linkReady = false }; negotiatedPayload = 20; if (hasConnectPermissions()) gatt?.disconnect(); gatt?.close(); gatt = null }

    private fun scheduleWeatherRefresh(delayMs: Long? = null) {
        if (destroyed) return
        reconnectHandler.removeCallbacks(weatherRefresh)
        val config = PhoneWeatherProxy.load(this) ?: return
        val delay = delayMs ?: config.refreshMinutes * 60_000L
        reconnectHandler.postDelayed(weatherRefresh, delay)
    }

    private fun fetchPhoneWeather() {
        if (!linkReady || weatherFetchInFlight) return
        val config = PhoneWeatherProxy.load(this) ?: return
        weatherFetchInFlight = true
        weatherExecutor.execute {
            val frame = runCatching { PhoneWeatherProxy.fetch(config) }.getOrNull()
            reconnectHandler.post {
                if (destroyed) return@post
                weatherFetchInFlight = false
                if (frame != null && linkReady) write(frame)
                scheduleWeatherRefresh(if (frame == null) 5 * 60_000L else null)
            }
        }
    }
    private fun hasConnectPermissions() = Build.VERSION.SDK_INT < 31 || (ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_SCAN) == PackageManager.PERMISSION_GRANTED && ActivityCompat.checkSelfPermission(this, Manifest.permission.BLUETOOTH_CONNECT) == PackageManager.PERMISSION_GRANTED)
    private fun createChannel() { getSystemService(NotificationManager::class.java).createNotificationChannel(NotificationChannel(CHANNEL, "Watch connection", NotificationManager.IMPORTANCE_LOW)) }
    private fun update(text: String) { getSystemService(NotificationManager::class.java).notify(7, connectionNotification(text)) }
    private fun connectionNotification(text: String) = NotificationCompat.Builder(this, CHANNEL).setSmallIcon(android.R.drawable.stat_sys_data_bluetooth).setContentTitle("Nightglass").setContentText(text).setOngoing(true).setContentIntent(PendingIntent.getActivity(this, 0, Intent(this, dev.nightglass.companion.MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)).build()
}
