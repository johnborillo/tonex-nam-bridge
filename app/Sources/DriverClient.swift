// Talks to the NAM Bridge driver through CoreAudio custom properties.
// Mirrors engine/DriverProtocol.h; keep the two in sync.

import CoreAudio
import Foundation

enum DriverProtocol {
    static let deviceUID = "NAMBridge_Device_UID"
    static let propModel = fourCC("nbMD")
    static let propConfig = fourCC("nbCF")
    static let propStatus = fourCC("nbST")

    static func fourCC(_ s: String) -> AudioObjectPropertySelector {
        s.utf8.reduce(0) { ($0 << 8) | AudioObjectPropertySelector($1) }
    }
}

struct DriverStatus: Equatable {
    var loaded = false
    var modelName = ""
    var fileName = ""
    var architecture = ""
    var modelRate = 0.0
    var hostRate = 0.0
    var resampling = false
    var inputGainDb = 0.0
    var outputGainDb = 0.0
    var inputSelect = 0
    var inPeakDb = -200.0
    var outPeakDb = -200.0
    var running = false
    var lastError = ""
    var version = ""
    var hasLoudness = false
    var loudnessDb = 0.0
}

enum DriverError: LocalizedError {
    case notInstalled
    case status(OSStatus, String)
    case read(String)

    var errorDescription: String? {
        switch self {
        case .notInstalled: return "NAM Bridge driver is not installed (run scripts/install-driver.sh)."
        case let .status(st, what): return "\(what) failed (OSStatus \(st))."
        case let .read(msg): return msg
        }
    }
}

final class DriverClient {
    private func address(_ sel: AudioObjectPropertySelector,
                         _ scope: AudioObjectPropertyScope = kAudioObjectPropertyScopeGlobal) -> AudioObjectPropertyAddress {
        AudioObjectPropertyAddress(mSelector: sel, mScope: scope, mElement: kAudioObjectPropertyElementMain)
    }

    func findDevice() -> AudioObjectID? {
        var addr = address(kAudioHardwarePropertyDevices)
        var size: UInt32 = 0
        guard AudioObjectGetPropertyDataSize(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size) == noErr
        else { return nil }
        var ids = [AudioObjectID](repeating: 0, count: Int(size) / MemoryLayout<AudioObjectID>.size)
        guard AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &addr, 0, nil, &size, &ids) == noErr
        else { return nil }
        for id in ids where uid(of: id) == DriverProtocol.deviceUID { return id }
        return nil
    }

    private func uid(of id: AudioObjectID) -> String? {
        var addr = address(kAudioDevicePropertyDeviceUID)
        var value: Unmanaged<CFString>?
        var size = UInt32(MemoryLayout<Unmanaged<CFString>?>.size)
        guard AudioObjectGetPropertyData(id, &addr, 0, nil, &size, &value) == noErr, let v = value else { return nil }
        return v.takeRetainedValue() as String
    }

    private func getDict(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector) -> [String: Any]? {
        var addr = address(sel)
        var value: Unmanaged<CFPropertyList>?
        var size = UInt32(MemoryLayout<Unmanaged<CFPropertyList>?>.size)
        guard AudioObjectGetPropertyData(dev, &addr, 0, nil, &size, &value) == noErr, let v = value else { return nil }
        return v.takeRetainedValue() as? [String: Any]
    }

    private func setDict(_ dev: AudioObjectID, _ sel: AudioObjectPropertySelector, _ dict: [String: Any], _ what: String) throws {
        var addr = address(sel)
        var plist: CFPropertyList = dict as CFDictionary
        let st = withUnsafeMutablePointer(to: &plist) { ptr in
            AudioObjectSetPropertyData(dev, &addr, 0, nil, UInt32(MemoryLayout<CFPropertyList>.size), ptr)
        }
        if st != noErr { throw DriverError.status(st, what) }
    }

    func status() -> DriverStatus? {
        guard let dev = findDevice(), let d = getDict(dev, DriverProtocol.propStatus) else { return nil }
        var s = DriverStatus()
        s.loaded = d["loaded"] as? Bool ?? false
        s.modelName = d["modelName"] as? String ?? ""
        s.fileName = d["fileName"] as? String ?? ""
        s.architecture = d["architecture"] as? String ?? ""
        s.modelRate = d["modelRate"] as? Double ?? 0
        s.hostRate = d["hostRate"] as? Double ?? 0
        s.resampling = d["resampling"] as? Bool ?? false
        s.inputGainDb = d["inputGainDb"] as? Double ?? 0
        s.outputGainDb = d["outputGainDb"] as? Double ?? 0
        s.inputSelect = Int(d["inputSelect"] as? Double ?? 0)
        s.inPeakDb = d["inPeakDb"] as? Double ?? -200
        s.outPeakDb = d["outPeakDb"] as? Double ?? -200
        s.running = d["running"] as? Bool ?? false
        s.lastError = d["lastError"] as? String ?? ""
        s.version = d["version"] as? String ?? ""
        s.hasLoudness = d["hasLoudness"] as? Bool ?? false
        s.loudnessDb = d["loudnessDb"] as? Double ?? 0
        return s
    }

    func loadModel(url: URL) throws {
        guard let dev = findDevice() else { throw DriverError.notInstalled }
        guard let data = try? Data(contentsOf: url), let json = String(data: data, encoding: .utf8) else {
            throw DriverError.read("Could not read \(url.lastPathComponent).")
        }
        try setDict(dev, DriverProtocol.propModel,
                    ["json": json, "fileName": url.deletingPathExtension().lastPathComponent], "Loading model")
    }

    func unload() throws {
        guard let dev = findDevice() else { throw DriverError.notInstalled }
        try setDict(dev, DriverProtocol.propModel, ["unload": true], "Unloading model")
    }

    func setConfig(inputGainDb: Double? = nil, outputGainDb: Double? = nil, inputSelect: Int? = nil) throws {
        guard let dev = findDevice() else { throw DriverError.notInstalled }
        var d: [String: Any] = [:]
        if let v = inputGainDb { d["inputGainDb"] = v }
        if let v = outputGainDb { d["outputGainDb"] = v }
        if let v = inputSelect { d["inputSelect"] = Double(v) }
        try setDict(dev, DriverProtocol.propConfig, d, "Updating settings")
    }
}
