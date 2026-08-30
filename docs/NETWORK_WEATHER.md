# Network and weather service

Nightglass includes an asynchronous Wi-Fi station and current-weather service.
It starts disabled, performs no network work until explicitly enabled, and does
not contain a default SSID, password, location, API token, or developer-network
fallback.

## Provisioning boundary

The service API accepts an SSID and password as bounded byte spans. A future
on-watch provisioning screen may call that API, but it must use masked input,
must never echo the password in UI, logs, URLs, crash reports, or diagnostics,
and must require an explicit save action. The service exposes only the boolean
`credentials_configured`; stored credential text never enters its snapshot.

Credentials are stored as one checksummed, versioned NVS blob and copied into
ESP-IDF Wi-Fi RAM storage only while connecting. Clearing credentials erases
the blob, replaces the Wi-Fi driver's RAM configuration with an empty station
record, and stops the station. This protects against torn or malformed
settings and accidental disclosure, but it is **not encryption at rest**.
Production devices that need resistance to physical flash extraction must
enable ESP-IDF NVS encryption and secure boot/flash encryption as a separate
device-provisioning policy. Nightglass does not falsely claim those fuses are
enabled.

Location uses signed WGS84 latitude/longitude in millionths of a degree. The UI
should offer a human-friendly location picker or manual coordinate entry, show
the selected location before saving, and avoid retaining search history.

## Runtime behavior

- Station configuration uses `WIFI_STORAGE_RAM`; Nightglass owns the single NVS
  credential record rather than leaving duplicate Wi-Fi configuration behind.
- Disconnects use bounded exponential reconnect backoff.
- Open-Meteo current weather is fetched in a background task. Startup and UI
  rendering never wait for Wi-Fi or HTTP.
- Requests use `https://api.open-meteo.com/v1/forecast` with the ESP-IDF root
  certificate bundle. Certificate verification is never disabled; fetches wait
  for a valid RTC-derived system time so certificate dates can be checked.
- Requests select current temperature, apparent temperature, WMO weather code,
  wind speed, and day/night state. Metric and imperial units are explicit.
- Responses are limited to 4096 bytes and decoded with a bounded,
  allocation-free parser. Malformed or out-of-range data is rejected.
- Last-good readings remain available with age and stale flags when refreshes
  fail. Offline and error states remain explicit.
- Fetches are deferred while the display is blank or the system is sleeping.
  Wi-Fi modem power saving is enabled while awake. The power supervisor stops
  Wi-Fi before explicit light sleep as required by ESP-IDF, then the network
  worker restarts it through the same reconnect state machine after wake.

Open-Meteo requires no API key for this non-commercial endpoint. Provider terms
and attribution requirements must be reviewed before any commercial release.
