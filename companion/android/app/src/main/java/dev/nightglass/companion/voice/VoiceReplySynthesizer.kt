package dev.nightglass.companion.voice

import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import dev.nightglass.companion.protocol.NightglassProtocol
import java.io.File
import kotlin.math.ceil

/**
 * Turns one bounded OpenClaw response into a short 8 kHz mu-law stream for
 * the watch speaker. The platform TTS engine is used only as a synthesizer;
 * the temporary WAV is wiped and deleted as soon as the callback completes.
 */
class VoiceReplySynthesizer(
    context: Context,
    private val main: Handler = Handler(Looper.getMainLooper()),
) {
    private val appContext = context.applicationContext
    private val lock = Any()
    private var ready = false
    private var closed = false
    private var serial = 0L
    private var pending: Pending? = null
    private val tts = TextToSpeech(appContext) { status ->
        synchronized(lock) { ready = status == TextToSpeech.SUCCESS && !closed }
    }

    private data class Pending(
        val id: String,
        val file: File,
        val callback: (Result<ByteArray>) -> Unit,
    )

    init {
        tts.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
            override fun onStart(utteranceId: String) = Unit
            override fun onDone(utteranceId: String) = finish(utteranceId, null)
            @Deprecated("API compatibility")
            override fun onError(utteranceId: String) = finish(
                utteranceId, IllegalStateException("Android TTS failed"))
            override fun onError(utteranceId: String, errorCode: Int) = finish(
                utteranceId, IllegalStateException("Android TTS failed ($errorCode)"))
        })
    }

    /** Returns false when the platform TTS path is not ready or already busy. */
    fun synthesize(text: String, callback: (Result<ByteArray>) -> Unit): Boolean {
        val normalized = VoiceResponseQueue.asciiForWatch(text)
        if (normalized.isEmpty()) return false
        val spoken = normalized.toString(Charsets.US_ASCII)
            .take(MAX_SPOKEN_REPLY_CHARS)
        normalized.fill(0)
        if (spoken.isBlank()) return false
        val request = synchronized(lock) {
            if (!ready || closed || pending != null) return@synchronized null
            val id = "nightglass-tts-${++serial}"
            val file = runCatching {
                File.createTempFile("nightglass-tts-", ".wav", appContext.cacheDir)
            }.getOrNull() ?: return@synchronized null
            Pending(id, file, callback).also { pending = it }
        }
        if (request == null) return false
        val accepted = runCatching {
            tts.synthesizeToFile(spoken, Bundle(), request.file, request.id) ==
                TextToSpeech.SUCCESS
        }.getOrDefault(false)
        if (!accepted) {
            finish(request.id, IllegalStateException("Android TTS rejected the response"))
        }
        return accepted
    }

    fun close() {
        val abandoned = synchronized(lock) {
            closed = true
            ready = false
            pending.also { pending = null }
        }
        abandoned?.file?.delete()
        runCatching { tts.stop() }
        runCatching { tts.shutdown() }
    }

    private fun finish(id: String, failure: Throwable?) {
        val request = synchronized(lock) {
            val current = pending
            if (current == null || current.id != id) null else {
                pending = null
                current
            }
        } ?: return
        val result = if (failure != null) {
            Result.failure(failure)
        } else {
            runCatching {
                require(request.file.length() in 44..MAX_WAV_BYTES) { "oversized TTS WAV" }
                val fileBytes = request.file.readBytes()
                try {
                    pcmWavToMulaw8k(fileBytes)
                } finally {
                    fileBytes.fill(0)
                }
            }
        }
        request.file.delete()
        main.post { request.callback(result) }
    }

    private fun pcmWavToMulaw8k(wav: ByteArray): ByteArray {
        require(wav.size in 44..MAX_WAV_BYTES && ascii(wav, 0, 4) == "RIFF" &&
            ascii(wav, 8, 4) == "WAVE") { "invalid TTS WAV" }
        var offset = 12
        var sampleRate = 0
        var channels = 0
        var bits = 0
        var dataOffset = -1
        var dataBytes = 0
        while (offset + 8 <= wav.size) {
            val chunkSize = le32(wav, offset + 4)
            val body = offset + 8
            require(chunkSize >= 0 && chunkSize <= wav.size - body) { "invalid TTS chunk" }
            when (ascii(wav, offset, 4)) {
                "fmt " -> {
                    require(chunkSize >= 16) { "short TTS fmt" }
                    require(le16(wav, body) == 1) { "compressed TTS audio" }
                    channels = le16(wav, body + 2)
                    sampleRate = le32(wav, body + 4)
                    bits = le16(wav, body + 14)
                }
                "data" -> {
                    require(dataOffset < 0) { "duplicate TTS data" }
                    dataOffset = body
                    dataBytes = chunkSize
                }
            }
            val padded = chunkSize + (chunkSize and 1)
            require(padded <= wav.size - body) { "truncated TTS chunk" }
            offset = body + padded
        }
        require(sampleRate > 0 && channels in 1..2 && bits == 16 && dataOffset >= 0 &&
            dataBytes > 0 && dataBytes % (channels * 2) == 0) { "unsupported TTS WAV" }
        val sourceFrames = dataBytes / (channels * 2)
        val outputFrames = ceil(sourceFrames.toDouble() * 8_000.0 / sampleRate)
            .toLong().coerceIn(1, NightglassProtocol.MAX_SPOKEN_REPLY_BYTES.toLong()).toInt()
        val result = ByteArray(outputFrames)
        for (output in 0 until outputFrames) {
            val source = ((output.toLong() * sampleRate) / 8_000L)
                .coerceAtMost((sourceFrames - 1).toLong()).toInt()
            var sum = 0
            for (channel in 0 until channels) {
                sum += le16Signed(wav, dataOffset + (source * channels + channel) * 2)
            }
            result[output] = pcm16ToMulaw(sum / channels).toByte()
        }
        return result
    }

    private fun le16(bytes: ByteArray, at: Int): Int =
        (bytes[at].toInt() and 0xff) or ((bytes[at + 1].toInt() and 0xff) shl 8)

    private fun le16Signed(bytes: ByteArray, at: Int): Int {
        val value = le16(bytes, at)
        return if (value and 0x8000 != 0) value - 0x1_0000 else value
    }

    private fun le32(bytes: ByteArray, at: Int): Int =
        (bytes[at].toInt() and 0xff) or
            ((bytes[at + 1].toInt() and 0xff) shl 8) or
            ((bytes[at + 2].toInt() and 0xff) shl 16) or
            ((bytes[at + 3].toInt() and 0xff) shl 24)

    private fun ascii(bytes: ByteArray, at: Int, size: Int): String =
        bytes.copyOfRange(at, at + size).toString(Charsets.US_ASCII)

    private fun pcm16ToMulaw(sample: Int): Int {
        val bias = 0x84
        val clip = 32635
        val sign = if (sample < 0) 0x80 else 0
        var magnitude = if (sample < 0) -sample else sample
        magnitude = magnitude.coerceAtMost(clip) + bias
        var exponent = 7
        var mask = 0x4000
        while (exponent > 0 && magnitude and mask == 0) {
            mask = mask ushr 1
            exponent--
        }
        val mantissa = (magnitude ushr (exponent + 3)) and 0x0f
        return (sign or (exponent shl 4) or mantissa).inv() and 0xff
    }

    companion object {
        private const val MAX_SPOKEN_REPLY_CHARS = 480
        private const val MAX_WAV_BYTES = 1_048_576
    }
}
