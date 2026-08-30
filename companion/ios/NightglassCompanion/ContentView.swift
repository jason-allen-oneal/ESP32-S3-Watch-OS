import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var bluetooth: NightglassBluetoothClient

    var body: some View {
        NavigationStack {
            Form {
                Section("Watch") {
                    LabeledContent("Bluetooth", value: bluetooth.state.description)
                    LabeledContent("Connection", value: bluetooth.connection.description)
                    if let status = bluetooth.watchStatus {
                        LabeledContent("Protocol", value: String(status.protocolVersion))
                        LabeledContent("Inbox", value: String(status.inboxCount))
                        LabeledContent("Encrypted", value: status.encrypted ? "Yes" : "No")
                        LabeledContent("Bonded", value: status.bonded ? "Yes" : "No")
                    }
                    Button(bluetooth.connection.isActive ? "Disconnect" : "Find Nightglass") {
                        bluetooth.connection.isActive ? bluetooth.disconnect() : bluetooth.startScanning()
                    }
                    .disabled(!bluetooth.state.canScan)
                }

                Section("iPhone boundary") {
                    Text("This app configures the custom Nightglass service. iOS does not let ordinary apps copy every system notification or control every app's media session.")
                        .font(.footnote)
                    NavigationLink("Provisioning status") {
                        ProvisioningView()
                    }
                }

                if let message = bluetooth.userMessage {
                    Section("Status") { Text(message) }
                }
            }
            .navigationTitle("Nightglass")
        }
    }
}

private struct ProvisioningView: View {
    @EnvironmentObject private var bluetooth: NightglassBluetoothClient
    @State private var ssid = ""
    @State private var password = ""
    @State private var enabled = true
    @State private var metric = false
    @State private var locationConfigured = false
    @State private var latitude = 0.0
    @State private var longitude = 0.0
    @State private var refreshMinutes = 30

    var body: some View {
        Form {
            Section("Wi-Fi") {
                TextField("SSID", text: $ssid)
                    .textInputAutocapitalization(.never)
                SecureField("Password", text: $password)
                Button("Send credentials") {
                    bluetooth.provisionWiFi(ssid: ssid, password: password)
                    password = ""
                }
                Button("Clear watch Wi-Fi", role: .destructive) { bluetooth.clearWiFi() }
            }
            Section("Network and weather") {
                Toggle("Enabled", isOn: $enabled)
                Toggle("Metric units", isOn: $metric)
                Toggle("Use coordinates", isOn: $locationConfigured)
                if locationConfigured {
                    TextField("Latitude", value: $latitude, format: .number)
                        .keyboardType(.numbersAndPunctuation)
                    TextField("Longitude", value: $longitude, format: .number)
                        .keyboardType(.numbersAndPunctuation)
                }
                Stepper("Refresh: \(refreshMinutes) min", value: $refreshMinutes, in: 15...360)
                Button("Apply settings") {
                    bluetooth.updateNetwork(enabled: enabled, locationConfigured: locationConfigured,
                                            metric: metric, refreshMinutes: UInt16(refreshMinutes),
                                            latitude: latitude, longitude: longitude)
                }
            } footer: {
                Text("Credentials are sent over the encrypted GATT write and are not logged or retained by this app.")
            }
        }
        .navigationTitle("Settings")
    }
}
