package dev.nightglass.companion.weather

import org.junit.Assert.*
import org.junit.Test

class PhoneWeatherProxyTest {
    @Test fun parsesBoundedCurrentWeather() {
        val reading = PhoneWeatherProxy.parse("""{
          "current":{"temperature_2m":72.4,"apparent_temperature":74.1,
          "weather_code":3,"wind_speed_10m":8.7,"is_day":1}}
        """)
        assertNotNull(reading)
        assertEquals(72.4, reading!!.temperature, 0.01)
        assertEquals(3, reading.code)
        assertTrue(reading.isDay)
    }

    @Test fun rejectsMissingDuplicateAndOutOfRangeValues() {
        assertNull(PhoneWeatherProxy.parse("{}"))
        assertNull(PhoneWeatherProxy.parse("""{"current":{"temperature_2m":1,
          "temperature_2m":2,"apparent_temperature":1,"weather_code":0,
          "wind_speed_10m":0,"is_day":1}}"""))
        assertNull(PhoneWeatherProxy.parse("""{"current":{"temperature_2m":999,
          "apparent_temperature":1,"weather_code":0,"wind_speed_10m":0,"is_day":1}}"""))
        assertNull(PhoneWeatherProxy.parse("x".repeat(4097)))
    }
}
