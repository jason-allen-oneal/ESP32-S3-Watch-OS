import SwiftUI

@main
struct NightglassCompanionApp: App {
    @StateObject private var bluetooth = NightglassBluetoothClient()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(bluetooth)
        }
    }
}
