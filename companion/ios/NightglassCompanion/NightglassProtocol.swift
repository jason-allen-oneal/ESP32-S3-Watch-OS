import CoreBluetooth
import Foundation

enum NightglassProtocol {
    static let version: UInt8 = 1
    static let service = CBUUID(string: "7A3B4001-6B6F-4F72-726F-772D6E696768")
    static let status = CBUUID(string: "7A3B4002-6B6F-4F72-726F-772D6E696768")
    static let phoneToWatch = CBUUID(string: "7A3B4003-6B6F-4F72-726F-772D6E696768")
    static let watchToPhone = CBUUID(string: "7A3B4004-6B6F-4F72-726F-772D6E696768")

    enum FrameError: LocalizedError {
        case invalidSSID, invalidPassword, invalidRefresh, invalidCoordinates
        var errorDescription: String? {
            switch self {
            case .invalidSSID: "SSID must contain 1–32 UTF-8 bytes."
            case .invalidPassword: "Password must contain at most 64 UTF-8 bytes."
            case .invalidRefresh: "Refresh interval must be between 15 and 360 minutes."
            case .invalidCoordinates: "Latitude or longitude is out of range."
            }
        }
    }

    static func wifiCredentials(ssid: String, password: String) throws -> Data {
        let ssidBytes = Array(ssid.utf8), passwordBytes = Array(password.utf8)
        guard (1...32).contains(ssidBytes.count) else { throw FrameError.invalidSSID }
        guard passwordBytes.count <= 64 else { throw FrameError.invalidPassword }
        return Data([version, 0x20, UInt8(ssidBytes.count), UInt8(passwordBytes.count)] + ssidBytes + passwordBytes)
    }

    static func networkSettings(enabled: Bool, locationConfigured: Bool, metric: Bool,
                                refreshMinutes: UInt16, latitude: Double, longitude: Double) throws -> Data {
        guard (15...360).contains(Int(refreshMinutes)) else { throw FrameError.invalidRefresh }
        guard (-90...90).contains(latitude), (-180...180).contains(longitude) else { throw FrameError.invalidCoordinates }
        let flags: UInt8 = (enabled ? 1 : 0) | (locationConfigured ? 2 : 0)
        let latitudeE6 = Int32((latitude * 1_000_000).rounded())
        let longitudeE6 = Int32((longitude * 1_000_000).rounded())
        var frame = Data([version, 0x21, flags, metric ? 1 : 0])
        appendLittleEndian(refreshMinutes, to: &frame)
        appendLittleEndian(UInt32(bitPattern: latitudeE6), to: &frame)
        appendLittleEndian(UInt32(bitPattern: longitudeE6), to: &frame)
        return frame
    }

    static let clearWiFi = Data([version, 0x22])

    private static func appendLittleEndian<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var littleEndian = value.littleEndian
        withUnsafeBytes(of: &littleEndian) { data.append(contentsOf: $0) }
    }
}

struct NightglassWatchStatus: Equatable {
    let protocolVersion: UInt8
    let linkState: UInt8
    let inboxCount: UInt8
    let encrypted: Bool
    let bonded: Bool

    init?(data: Data) {
        guard data.count == 5 else { return nil }
        protocolVersion = data[data.startIndex]
        linkState = data[data.startIndex + 1]
        inboxCount = data[data.startIndex + 2]
        encrypted = data[data.startIndex + 3] != 0
        bonded = data[data.startIndex + 4] != 0
    }
}

enum NightglassWatchCommand: Equatable {
    case media(sequence: UInt8, command: UInt8)
    case dismiss(sequence: UInt8, notificationID: UInt32)
    case markRead(sequence: UInt8, notificationID: UInt32)

    init?(data: Data) {
        let bytes = [UInt8](data)
        guard bytes.first == NightglassProtocol.version, bytes.count >= 2 else { return nil }
        switch bytes[1] {
        case 0x10 where bytes.count == 4:
            self = .media(sequence: bytes[2], command: bytes[3])
        case 0x11 where bytes.count == 7:
            self = .dismiss(sequence: bytes[2], notificationID: Self.readUInt32(bytes, at: 3))
        case 0x12 where bytes.count == 7:
            self = .markRead(sequence: bytes[2], notificationID: Self.readUInt32(bytes, at: 3))
        default:
            return nil
        }
    }

    private static func readUInt32(_ bytes: [UInt8], at offset: Int) -> UInt32 {
        UInt32(bytes[offset]) | (UInt32(bytes[offset + 1]) << 8) |
            (UInt32(bytes[offset + 2]) << 16) | (UInt32(bytes[offset + 3]) << 24)
    }
}
