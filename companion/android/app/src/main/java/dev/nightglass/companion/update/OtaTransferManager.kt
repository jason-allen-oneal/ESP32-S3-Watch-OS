package dev.nightglass.companion.update

import android.content.ContentResolver
import dev.nightglass.companion.protocol.NightglassProtocol
import java.io.InputStream
import java.util.concurrent.ScheduledExecutorService
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit

class OtaTransferManager(
    private val resolver: ContentResolver,
    private val executor: ScheduledExecutorService,
    private val negotiatedPayload: () -> Int,
    private val send: (ByteArray) -> Boolean,
    private val report: (Progress) -> Unit,
) {
    data class Progress(val active: Boolean, val complete: Boolean, val percent: Int,
                        val detail: String)

    private var selected: PreparedOtaPackage? = null
    private var image: InputStream? = null
    private var imageOffset = 0L
    private var processing = false
    private var aborting = false
    private var foreignSession: ULong? = null
    private var timeout: ScheduledFuture<*>? = null
    private var ackGeneration = 0
    private var retryCount = 0
    private val confirmation = OtaConfirmationTracker()
    private var lastPercent = -1

    @Synchronized fun start(uris: List<android.net.Uri>) {
        if (processing || selected != null) {
            emit(true, false, maxOf(lastPercent, 0), "An update is already active")
            return
        }
        processing = true
        executor.execute {
            val result = runCatching { OtaPackageLoader.load(resolver, uris) }
            synchronized(this) { processing = false; selected = result.getOrNull() }
            val pkg = result.getOrNull()
            if (pkg == null) {
                fail("Update package rejected: ${result.exceptionOrNull()?.message ?: "invalid package"}")
            } else if (negotiatedPayload() < 244) {
                emit(true, false, 0, "Waiting for secure MTU 247 link")
            } else {
                emit(true, false, 0, "Checking watch update state")
                sendAndAwait(NightglassProtocol.otaStatusQuery(pkg.session))
            }
        }
    }

    @Synchronized fun resumeLink() {
        val pkg = selected ?: return
        cancelTimeout()
        if (negotiatedPayload() < 244) {
            emit(true, false, maxOf(lastPercent, 0), "Nightglass MTU 247 is required")
            return
        }
        if (aborting) sendAndAwait(NightglassProtocol.otaAbort(pkg.session))
        else sendAndAwait(NightglassProtocol.otaStatusQuery(pkg.session))
    }

    @Synchronized fun linkLost() {
        cancelTimeout()
        selected?.let { emit(true, false, maxOf(lastPercent, 0),
            "Transfer paused; reconnect within 30 seconds to resume") }
    }

    @Synchronized fun cancel() {
        val pkg = selected ?: return
        aborting = true
        closeImage()
        emit(true, false, maxOf(lastPercent, 0), "Waiting for watch abort acknowledgement")
        if (negotiatedPayload() >= 244) sendAndAwait(NightglassProtocol.otaAbort(pkg.session))
    }

    fun onStatus(status: NightglassProtocol.OtaStatus) {
        executor.execute { handleStatus(status) }
    }

    @Synchronized private fun handleStatus(status: NightglassProtocol.OtaStatus) {
        val pkg = selected ?: return
        cancelTimeout()
        if (processing) return
        processing = true
        try {
            val foreign = foreignSession
            if (foreign != null) {
                if (status.session != foreign) return fail("Watch update session changed unexpectedly")
                if (status.result != 0) return fail("Unable to clear prior watch update")
                if (status.state == 1) {
                    foreignSession = null
                    return sendAndAwait(NightglassProtocol.otaStatusQuery(pkg.session))
                }
                return sendAndAwait(NightglassProtocol.otaAbort(foreign))
            }
            if (status.session != pkg.session) {
                foreignSession = status.session
                emit(true, false, maxOf(lastPercent, 0), "Clearing prior watch update session")
                return sendAndAwait(NightglassProtocol.otaAbort(status.session))
            }
            if (status.result == 9) {
                emit(true, false, 100, "Confirm INSTALL on the watch")
                confirmation.begin()
                scheduleConfirmationPoll()
                return
            }
            confirmation.reset()
            if (status.result != 0) return fail("Watch rejected update operation (${status.result})")
            if (aborting) {
                if (status.state == 1) {
                    closeImage(); selected = null; aborting = false
                    emit(false, false, 0, "Update aborted by watch")
                } else sendAndAwait(NightglassProtocol.otaAbort(pkg.session))
                return
            }
            when (status.state) {
                0 -> fail("Watch update backend is disabled")
                1 -> {
                    emit(true, false, 0, "Authorizing signed update")
                    sendAndAwait(NightglassProtocol.otaBegin(pkg.session, pkg.manifest))
                }
                2 -> {
                    if (status.expectedBytes != pkg.manifest.imageSize ||
                        status.receivedBytes > pkg.manifest.imageSize) {
                        aborting = true
                        return sendAndAwait(NightglassProtocol.otaAbort(pkg.session))
                    }
                    if (status.receivedBytes == pkg.manifest.imageSize) {
                        emit(true, false, 100, "Requesting watch confirmation")
                        sendAndAwait(NightglassProtocol.otaFinish(pkg.session))
                    } else {
                        val data = readChunk(pkg, status.receivedBytes)
                        if (data.isEmpty()) return fail("Firmware ended before manifest size")
                        val percent = ((status.receivedBytes * 100) / pkg.manifest.imageSize).toInt()
                        emit(true, false, percent, "Transferring signed firmware")
                        sendAndAwait(NightglassProtocol.otaData(pkg.session,
                            status.receivedBytes, data))
                    }
                }
                3 -> {
                    closeImage(); selected = null
                    emit(false, true, 100,
                        "Update validated; controlled reboot is still required")
                }
                4 -> sendAndAwait(NightglassProtocol.otaAbort(pkg.session))
                else -> fail("Unknown watch update state")
            }
        } catch (error: Exception) {
            fail("Update stopped: ${error.message ?: error.javaClass.simpleName}")
        } finally { processing = false }
    }

    @Synchronized private fun sendAndAwait(frame: ByteArray) {
        if (!send(frame)) {
            emit(true, false, maxOf(lastPercent, 0), "Waiting for secure Nightglass link")
            return
        }
        retryCount = 0
        scheduleTimeout()
    }

    @Synchronized private fun scheduleTimeout() {
        timeout?.cancel(false)
        val generation = ++ackGeneration
        timeout = executor.schedule({
            synchronized(this) {
                val pkg = selected ?: return@synchronized
                if (generation != ackGeneration) return@synchronized
                if (++retryCount > 3) fail("Watch update acknowledgement timed out")
                else if (!send(NightglassProtocol.otaStatusQuery(pkg.session)))
                    emit(true, false, maxOf(lastPercent, 0), "Waiting for secure Nightglass link")
                else scheduleTimeout()
            }
        }, 10, TimeUnit.SECONDS)
    }

    @Synchronized private fun scheduleConfirmationPoll() {
        timeout?.cancel(false)
        val generation = ++ackGeneration
        timeout = executor.schedule({
            synchronized(this) {
                val pkg = selected ?: return@synchronized
                if (generation != ackGeneration || !confirmation.active) {
                    return@synchronized
                }
                if (!confirmation.allowNextPoll()) {
                    fail("Watch confirmation timed out")
                } else {
                    if (!send(NightglassProtocol.otaStatusQuery(pkg.session))) {
                        emit(true, false, maxOf(lastPercent, 0),
                            "Waiting for secure Nightglass link")
                    }
                    scheduleConfirmationPoll()
                }
            }
        }, 10, TimeUnit.SECONDS)
    }

    private fun readChunk(pkg: PreparedOtaPackage, offset: Long): ByteArray {
        if (image == null || imageOffset != offset) {
            closeImage()
            image = resolver.openInputStream(pkg.firmware) ?: error("Firmware stream unavailable")
            var remaining = offset
            while (remaining > 0) {
                val skipped = image!!.skip(remaining)
                if (skipped > 0) remaining -= skipped
                else { if (image!!.read() < 0) error("Firmware resume offset is beyond end"); remaining-- }
            }
            imageOffset = offset
        }
        val maximum = minOf(230L, pkg.manifest.imageSize - offset).toInt()
        val buffer = ByteArray(maximum)
        var count = 0
        while (count < maximum) {
            val read = image!!.read(buffer, count, maximum - count)
            if (read < 0) break
            if (read == 0) error("Firmware stream stalled")
            count += read
        }
        imageOffset += count
        return if (count == buffer.size) buffer else buffer.copyOf(count)
    }

    @Synchronized private fun fail(detail: String) {
        val pkg = selected
        if (pkg != null && negotiatedPayload() >= 244) {
            send(NightglassProtocol.otaAbort(pkg.session))
        }
        cancelTimeout(); closeImage(); selected = null; aborting = false; foreignSession = null
        confirmation.reset()
        emit(false, false, 0, detail)
    }
    private fun cancelTimeout() { ++ackGeneration; timeout?.cancel(false); timeout = null }
    private fun emit(active: Boolean, complete: Boolean, percent: Int, detail: String) {
        if (percent == lastPercent && detail == "Transferring signed firmware") return
        lastPercent = percent
        report(Progress(active, complete, percent, detail))
    }
    private fun closeImage() { image?.close(); image = null; imageOffset = 0 }
}
