import CoreBluetooth
import Foundation

@MainActor
final class NightglassBluetoothClient: NSObject, ObservableObject {
    enum RadioState {
        case unknown, resetting, unsupported, unauthorized, poweredOff, poweredOn

        var canScan: Bool { self == .poweredOn }
        var description: String {
            switch self {
            case .unknown: "Unknown"
            case .resetting: "Resetting"
            case .unsupported: "Unsupported"
            case .unauthorized: "Permission required"
            case .poweredOff: "Off"
            case .poweredOn: "On"
            }
        }
    }

    enum ConnectionState {
        case idle, scanning, connecting, discovering, ready
        var isActive: Bool { self != .idle }
        var description: String {
            switch self {
            case .idle: "Not connected"
            case .scanning: "Scanning"
            case .connecting: "Connecting"
            case .discovering: "Discovering service"
            case .ready: "Ready"
            }
        }
    }

    @Published private(set) var state: RadioState = .unknown
    @Published private(set) var connection: ConnectionState = .idle
    @Published private(set) var watchStatus: NightglassWatchStatus?
    @Published private(set) var userMessage: String?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var statusCharacteristic: CBCharacteristic?
    private var inboundCharacteristic: CBCharacteristic?
    private var outboundCharacteristic: CBCharacteristic?

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func startScanning() {
        guard state.canScan, connection == .idle else { return }
        userMessage = nil
        connection = .scanning
        central.scanForPeripherals(withServices: [NightglassProtocol.service], options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func disconnect() {
        central.stopScan()
        if let peripheral { central.cancelPeripheralConnection(peripheral) }
        resetConnection()
    }

    func provisionWiFi(ssid: String, password: String) {
        do { try write(NightglassProtocol.wifiCredentials(ssid: ssid, password: password)) }
        catch { userMessage = error.localizedDescription }
    }

    func updateNetwork(enabled: Bool, locationConfigured: Bool, metric: Bool,
                       refreshMinutes: UInt16, latitude: Double, longitude: Double) {
        do {
            try write(NightglassProtocol.networkSettings(enabled: enabled, locationConfigured: locationConfigured,
                                                         metric: metric, refreshMinutes: refreshMinutes,
                                                         latitude: latitude, longitude: longitude))
        } catch { userMessage = error.localizedDescription }
    }

    func clearWiFi() { try? write(NightglassProtocol.clearWiFi) }

    private func write(_ data: Data) throws {
        guard connection == .ready, let peripheral, let inboundCharacteristic else {
            throw NSError(domain: "Nightglass", code: 1, userInfo: [NSLocalizedDescriptionKey: "Connect to Nightglass first."])
        }
        guard data.count <= peripheral.maximumWriteValueLength(for: .withResponse) else {
            throw NSError(domain: "Nightglass", code: 2, userInfo: [NSLocalizedDescriptionKey: "Provisioning frame is too large for this connection."])
        }
        peripheral.writeValue(data, for: inboundCharacteristic, type: .withResponse)
        userMessage = "Settings queued for Nightglass."
    }

    private func resetConnection(message: String? = nil) {
        peripheral = nil
        statusCharacteristic = nil
        inboundCharacteristic = nil
        outboundCharacteristic = nil
        watchStatus = nil
        connection = .idle
        userMessage = message
    }
}

extension NightglassBluetoothClient: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            state = switch central.state {
            case .unknown: .unknown
            case .resetting: .resetting
            case .unsupported: .unsupported
            case .unauthorized: .unauthorized
            case .poweredOff: .poweredOff
            case .poweredOn: .poweredOn
            @unknown default: .unknown
            }
            if !state.canScan { resetConnection() }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String: Any], rssi RSSI: NSNumber) {
        Task { @MainActor in
            guard connection == .scanning else { return }
            central.stopScan()
            self.peripheral = peripheral
            peripheral.delegate = self
            connection = .connecting
            central.connect(peripheral)
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            connection = .discovering
            peripheral.discoverServices([NightglassProtocol.service])
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in resetConnection(message: "Could not connect to Nightglass.") }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in resetConnection(message: error == nil ? nil : "Nightglass disconnected.") }
    }
}

extension NightglassBluetoothClient: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        Task { @MainActor in
            guard error == nil, let service = peripheral.services?.first(where: { $0.uuid == NightglassProtocol.service }) else {
                resetConnection(message: "Nightglass service was not found.")
                central.cancelPeripheralConnection(peripheral)
                return
            }
            peripheral.discoverCharacteristics([NightglassProtocol.status, NightglassProtocol.phoneToWatch, NightglassProtocol.watchToPhone], for: service)
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        Task { @MainActor in
            guard error == nil else {
                resetConnection(message: "Nightglass characteristics could not be discovered.")
                central.cancelPeripheralConnection(peripheral)
                return
            }
            for characteristic in service.characteristics ?? [] {
                switch characteristic.uuid {
                case NightglassProtocol.status: statusCharacteristic = characteristic
                case NightglassProtocol.phoneToWatch: inboundCharacteristic = characteristic
                case NightglassProtocol.watchToPhone: outboundCharacteristic = characteristic
                default: break
                }
            }
            guard let statusCharacteristic, inboundCharacteristic != nil, let outboundCharacteristic else {
                resetConnection(message: "Nightglass protocol v1 is incomplete on this device.")
                central.cancelPeripheralConnection(peripheral)
                return
            }
            peripheral.readValue(for: statusCharacteristic)
            peripheral.setNotifyValue(true, for: statusCharacteristic)
            peripheral.setNotifyValue(true, for: outboundCharacteristic)
            connection = .ready
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        Task { @MainActor in
            guard error == nil, let data = characteristic.value else { return }
            if characteristic.uuid == NightglassProtocol.status {
                guard let parsed = NightglassWatchStatus(data: data) else {
                    userMessage = "Nightglass sent an invalid status frame."
                    return
                }
                watchStatus = parsed
                if parsed.protocolVersion != NightglassProtocol.version {
                    userMessage = "Nightglass protocol version is not supported by this app."
                }
            } else if characteristic.uuid == NightglassProtocol.watchToPhone {
                // Parse command metadata only. Never print payload bytes or notification content.
                _ = NightglassWatchCommand(data: data)
            }
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        Task { @MainActor in
            guard characteristic.uuid == NightglassProtocol.phoneToWatch else { return }
            userMessage = error == nil ? "Nightglass received the settings frame." : "Nightglass did not receive the settings frame."
        }
    }
}
