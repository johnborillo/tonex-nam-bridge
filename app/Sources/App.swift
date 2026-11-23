// NAM Bridge menu-bar app: pick the NAM capture the virtual "amp" runs, set levels, watch meters.

import AppKit
import SwiftUI
import UniformTypeIdentifiers

@MainActor
final class BridgeModel: ObservableObject {
    @Published var status: DriverStatus?
    @Published var message = ""
    @Published var inputGain = 0.0
    @Published var outputGain = 0.0
    @Published var inputSelect = 0
    @Published var recents: [URL] = []

    private let client = DriverClient()
    private var timer: Timer?
    private var settingsSynced = false
    private let recentsKey = "recentModels"

    init() {
        recents = (UserDefaults.standard.stringArray(forKey: recentsKey) ?? []).map { URL(fileURLWithPath: $0) }
            .filter { FileManager.default.fileExists(atPath: $0.path) }
        refresh()
        setPolling(fast: false)
    }

    /// Fast meter updates only while the menu is open; otherwise a slow check keeps the
    /// menu-bar icon (model loaded or not) current without burning CPU.
    func setPolling(fast: Bool) {
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: fast ? 0.2 : 5.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.refresh() }
        }
        if fast { refresh() }
    }

    func refresh() {
        let s = client.status()
        if s != status { status = s }
        if let s, !settingsSynced {
            inputGain = s.inputGainDb
            outputGain = s.outputGainDb
            inputSelect = s.inputSelect
            settingsSynced = true
        }
        if s == nil { settingsSynced = false }
    }

    func chooseModel() {
        let panel = NSOpenPanel()
        panel.title = "Choose a NAM capture"
        panel.allowedContentTypes = [UTType(filenameExtension: "nam") ?? .json]
        panel.allowsMultipleSelection = false
        NSApp.activate(ignoringOtherApps: true)
        if panel.runModal() == .OK, let url = panel.url { load(url) }
    }

    func load(_ url: URL) {
        do {
            try client.loadModel(url: url)
            refresh()
            if let err = status?.lastError, !err.isEmpty {
                message = err
            } else {
                message = ""
                remember(url)
            }
        } catch {
            message = error.localizedDescription
        }
    }

    func unload() {
        do { try client.unload(); message = "" } catch { message = error.localizedDescription }
        refresh()
    }

    func pushSettings() {
        do {
            try client.setConfig(inputGainDb: inputGain, outputGainDb: outputGain, inputSelect: inputSelect)
        } catch {
            message = error.localizedDescription
        }
    }

    private func remember(_ url: URL) {
        recents.removeAll { $0.path == url.path }
        recents.insert(url, at: 0)
        recents = Array(recents.prefix(10))
        UserDefaults.standard.set(recents.map(\.path), forKey: recentsKey)
    }
}

struct Meter: View {
    let label: String
    let db: Double

    var body: some View {
        let clamped = max(-60, min(0, db))
        let fraction = (clamped + 60) / 60
        HStack(spacing: 8) {
            Text(label).font(.caption).frame(width: 28, alignment: .leading)
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    RoundedRectangle(cornerRadius: 2).fill(Color.secondary.opacity(0.2))
                    RoundedRectangle(cornerRadius: 2)
                        .fill(db > -0.1 ? Color.red : (db > -6 ? Color.orange : Color.green))
                        .frame(width: geo.size.width * fraction)
                }
            }
            .frame(height: 8)
            Text(db <= -199 ? "-inf" : String(format: "%.1f", db))
                .font(.caption.monospacedDigit()).frame(width: 40, alignment: .trailing)
        }
    }
}

struct ContentView: View {
    @ObservedObject var model: BridgeModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("NAM Bridge").font(.headline)
                Spacer()
                if let s = model.status {
                    Circle().fill(s.running ? Color.green : Color.secondary).frame(width: 8, height: 8)
                    Text(s.running ? "In use" : "Idle").font(.caption).foregroundStyle(.secondary)
                }
            }

            if let s = model.status {
                VStack(alignment: .leading, spacing: 2) {
                    Text(s.loaded ? s.modelName : "No model loaded")
                        .font(.body.weight(.medium)).lineLimit(2)
                    if s.loaded {
                        Text("\(s.architecture) · trained @ \(Int(s.modelRate)) Hz · device @ \(Int(s.hostRate)) Hz"
                            + (s.resampling ? " · resampling" : ""))
                            .font(.caption).foregroundStyle(.secondary)
                    } else {
                        Text("The NAM Bridge input stays silent until you load a capture.")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                }

                HStack {
                    Button("Load .nam…") { model.chooseModel() }
                    if !model.recents.isEmpty {
                        Menu("Recent") {
                            ForEach(model.recents, id: \.self) { url in
                                Button(url.deletingPathExtension().lastPathComponent) { model.load(url) }
                            }
                        }
                        .frame(width: 90)
                    }
                    Spacer()
                    if s.loaded { Button("Unload") { model.unload() } }
                }

                Divider()
                Meter(label: "In", db: s.inPeakDb)
                Meter(label: "Out", db: s.outPeakDb)

                Divider()
                gainRow("Input gain", value: $model.inputGain)
                gainRow("Output gain", value: $model.outputGain)
                HStack {
                    Text("Source channel").font(.caption)
                    Spacer()
                    Picker("", selection: $model.inputSelect) {
                        Text("Left").tag(0)
                        Text("Right").tag(1)
                        Text("L+R").tag(2)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 160)
                    .onChange(of: model.inputSelect) { _, _ in model.pushSettings() }
                }

                if !s.lastError.isEmpty || !model.message.isEmpty {
                    Text(model.message.isEmpty ? s.lastError : model.message)
                        .font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true)
                }

                Divider()
                Text("In ToneX Modeler, choose “NAM Bridge” as the audio device: output 1 → amp, input 1 ← amp.")
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            } else {
                Text("The NAM Bridge driver isn't running.")
                Text("Install it with scripts/install-driver.sh, then reopen this menu.")
                    .font(.caption).foregroundStyle(.secondary)
            }

            HStack {
                if let v = model.status?.version { Text("driver \(v)").font(.caption2).foregroundStyle(.tertiary) }
                Spacer()
                Button("Quit") { NSApp.terminate(nil) }
            }
        }
        .padding(14)
        .frame(width: 340)
        .onAppear { model.setPolling(fast: true) }
        .onDisappear { model.setPolling(fast: false) }
    }

    private func gainRow(_ title: String, value: Binding<Double>) -> some View {
        HStack {
            Text(title).font(.caption).frame(width: 74, alignment: .leading)
            Slider(value: value, in: -24...24, step: 0.5) { editing in
                if !editing { model.pushSettings() }
            }
            Text(String(format: "%+.1f dB", value.wrappedValue))
                .font(.caption.monospacedDigit()).frame(width: 56, alignment: .trailing)
        }
    }
}

@main
struct NAMBridgeApp: App {
    @StateObject private var model = BridgeModel()

    var body: some Scene {
        MenuBarExtra {
            ContentView(model: model)
        } label: {
            Image(systemName: model.status?.loaded == true ? "waveform.circle.fill" : "waveform.circle")
        }
        .menuBarExtraStyle(.window)
    }
}
