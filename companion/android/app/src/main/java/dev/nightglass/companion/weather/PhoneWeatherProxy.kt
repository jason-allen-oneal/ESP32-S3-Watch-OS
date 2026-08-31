package dev.nightglass.companion.weather

import android.content.Context
import dev.nightglass.companion.protocol.NightglassProtocol
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.net.URLEncoder
import java.nio.charset.StandardCharsets

object PhoneWeatherProxy {
    private const val PREFS = "nightglass_weather_proxy"
    private const val MAX_RESPONSE = 4096

    data class Config(val latitudeE6: Int, val longitudeE6: Int, val metric: Boolean,
                      val refreshMinutes: Int)
    data class Reading(val temperature: Double, val apparent: Double, val code: Int,
                       val wind: Double, val isDay: Boolean)

    fun save(context: Context, config: Config) {
        require(config.latitudeE6 in -90_000_000..90_000_000)
        require(config.longitudeE6 in -180_000_000..180_000_000)
        require(config.refreshMinutes in 15..360)
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
            .putInt("lat_e6", config.latitudeE6).putInt("lon_e6", config.longitudeE6)
            .putBoolean("metric", config.metric).putInt("refresh_min", config.refreshMinutes)
            .putBoolean("configured", true).apply()
    }

    fun load(context: Context): Config? {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        if (!prefs.getBoolean("configured", false)) return null
        val config = Config(prefs.getInt("lat_e6", 0), prefs.getInt("lon_e6", 0),
            prefs.getBoolean("metric", false), prefs.getInt("refresh_min", 30))
        return config.takeIf { it.latitudeE6 in -90_000_000..90_000_000 &&
            it.longitudeE6 in -180_000_000..180_000_000 && it.refreshMinutes in 15..360 }
    }

    fun fetch(config: Config): ByteArray {
        val latitude = config.latitudeE6 / 1_000_000.0
        val longitude = config.longitudeE6 / 1_000_000.0
        val tempUnit = if (config.metric) "celsius" else "fahrenheit"
        val windUnit = if (config.metric) "kmh" else "mph"
        val query = "latitude=${enc(latitude.toString())}&longitude=${enc(longitude.toString())}" +
            "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m,is_day" +
            "&temperature_unit=$tempUnit&wind_speed_unit=$windUnit&forecast_days=1"
        val connection = URL("https://api.open-meteo.com/v1/forecast?$query")
            .openConnection() as HttpURLConnection
        connection.connectTimeout = 8_000
        connection.readTimeout = 8_000
        connection.instanceFollowRedirects = false
        connection.requestMethod = "GET"
        connection.setRequestProperty("Accept", "application/json")
        connection.setRequestProperty("User-Agent", "Nightglass-Android/1")
        try {
            require(connection.responseCode == 200) { "weather HTTP status" }
            val body = ByteArrayOutputStream()
            connection.inputStream.use { input ->
                val buffer = ByteArray(1024)
                while (true) {
                    val count = input.read(buffer)
                    if (count < 0) break
                    require(body.size() + count <= MAX_RESPONSE) { "weather response too large" }
                    body.write(buffer, 0, count)
                }
            }
            val reading = parse(body.toString(StandardCharsets.UTF_8.name()))
                ?: error("invalid weather response")
            return NightglassProtocol.phoneWeather(System.currentTimeMillis() / 1000L,
                config.metric, reading.isDay, reading.temperature, reading.apparent,
                reading.code, reading.wind)
        } finally {
            connection.disconnect()
        }
    }

    fun parse(json: String): Reading? {
        if (json.isEmpty() || json.length > MAX_RESPONSE) return null
        fun number(key: String): Double? {
            val match = Regex("\\\"${Regex.escape(key)}\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)")
                .findAll(json).toList()
            return if (match.size == 1) match[0].groupValues[1].toDoubleOrNull() else null
        }
        val temperature = number("temperature_2m") ?: return null
        val apparent = number("apparent_temperature") ?: return null
        val codeValue = number("weather_code") ?: return null
        val wind = number("wind_speed_10m") ?: return null
        val dayValue = number("is_day") ?: return null
        if (temperature !in -150.0..150.0 || apparent !in -150.0..150.0 ||
            wind !in 0.0..500.0 || codeValue !in 0.0..999.0 || codeValue % 1.0 != 0.0 ||
            (dayValue != 0.0 && dayValue != 1.0)) return null
        return Reading(temperature, apparent, codeValue.toInt(), wind, dayValue == 1.0)
    }

    private fun enc(value: String) = URLEncoder.encode(value, StandardCharsets.UTF_8.name())
}
