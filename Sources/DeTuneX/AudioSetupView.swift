import CoreAudio
import SwiftUI

/// Audio device / channel / sample-rate / buffer configuration, latency
/// breakdown, gain staging and MIDI mapping.
struct AudioSetupView: View {
    @EnvironmentObject var engine: AudioEngine
    @EnvironmentObject var stats: LiveStats
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text("Audio Setup").font(.title2.bold())
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            .padding()

            Form {
                devicesSection
                timingSection
                latencySection
                gainSection
                midiSection
            }
            .formStyle(.grouped)
        }
        .frame(width: 560, height: 720)
    }

    // MARK: Devices

    private var devicesSection: some View {
        Section("Devices") {
            Picker("Input device", selection: inputUID) {
                ForEach(engine.inputDevices) { d in
                    Text("\(d.name)  (\(d.transportName), \(d.inputChannels) in)").tag(Optional(d.uid))
                }
            }
            if let dev = engine.currentInput {
                Picker("Input channel", selection: $engine.audio.inputChannel) {
                    ForEach(Array(AudioDevices.channelNames(dev, input: true).enumerated()), id: \.offset) { i, name in
                        Text(name).tag(i)
                    }
                }
            }
            Picker("Output device", selection: outputUID) {
                ForEach(engine.outputDevices) { d in
                    Text("\(d.name)  (\(d.transportName), \(d.outputChannels) out)").tag(Optional(d.uid))
                }
            }
            if let dev = engine.currentOutput {
                Picker("Output channels", selection: outputPair) {
                    ForEach(outputPairs(dev), id: \.self) { pair in
                        Text(pair.r >= 0 ? "\(pair.l + 1) + \(pair.r + 1)  (stereo, same signal)" : "\(pair.l + 1)  (mono)")
                            .tag(pair)
                    }
                }
            }
            if engine.hostInfo.usingAggregate {
                Label("Separate input and output devices run through a private aggregate device (output is the clock, input is drift-corrected). For the lowest latency use one interface for both.",
                      systemImage: "info.circle")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if let i = engine.currentInput, i.transport == kAudioDeviceTransportTypeBluetooth || i.transport == kAudioDeviceTransportTypeBluetoothLE {
                Label("Bluetooth input adds 100 ms or more of latency — use a wired interface.", systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundStyle(.orange)
            }
            if let o = engine.currentOutput, o.transport == kAudioDeviceTransportTypeBluetooth || o.transport == kAudioDeviceTransportTypeBluetoothLE {
                Label("Bluetooth output adds 100–200 ms of latency — use wired headphones or an interface.", systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundStyle(.orange)
            }
        }
    }

    // MARK: Sample rate & buffer

    private var timingSection: some View {
        Section {
            Picker("Sample rate", selection: $engine.audio.sampleRate) {
                ForEach(engine.commonSampleRates, id: \.self) { r in
                    Text(String(format: r.truncatingRemainder(dividingBy: 1000) == 0 ? "%.0f kHz" : "%.1f kHz", r / 1000)).tag(r)
                }
            }
            Picker("Buffer size", selection: $engine.audio.bufferFrames) {
                ForEach(engine.bufferChoices, id: \.self) { n in
                    let rate = engine.hostInfo.sampleRate > 0 ? engine.hostInfo.sampleRate : engine.audio.sampleRate
                    Text(String(format: "%ld samples  (%.2f ms)", n, Double(n) * 1000 / max(rate, 1))).tag(n)
                }
            }
            Picker("Output timing (IO cycle usage)", selection: $engine.audio.ioCycleUsage) {
                Text("100 %  — safe (macOS default)").tag(1.0)
                Text("75 %").tag(0.75)
                Text("50 %  — recommended").tag(0.5)
                Text("35 %").tag(0.35)
                Text("25 %  — aggressive").tag(0.25)
            }
            if engine.hostInfo.sampleRate > 0 {
                let saved = Double(engine.hostInfo.bufferFrames) - engine.hostInfo.outputBufferFrames
                LabeledContent("Output latency saved", value: String(format: "%.2f ms", saved * 1000 / engine.hostInfo.sampleRate))
            }
        } header: {
            Text("Timing")
        } footer: {
            Text("Buffer: smaller is faster but needs more CPU headroom. Start at 64, try 32 or 16; go back up if the dropout counter climbs. 96 kHz halves the time of each buffer and shortens converter latency.\nOutput timing: macOS normally starts processing at the beginning of each IO cycle. De-TuneX needs only a few % of the cycle, so letting macOS call it later removes up to most of one buffer of output latency. If you hear crackles, step back towards 100 %.")
        }
    }

    // MARK: Latency report

    private var latencySection: some View {
        Section("Latency (measured from the driver)") {
            let h = engine.hostInfo
            let ms: (Int) -> String = { f in h.sampleRate > 0 ? String(format: "%.2f ms", Double(f) * 1000 / h.sampleRate) : "—" }
            LabeledContent("Input converter + driver", value: ms(h.inputLatencyFrames))
            LabeledContent("Input buffer", value: ms(h.bufferFrames))
            LabeledContent("Output buffer (after timing trim)", value: h.sampleRate > 0 ? String(format: "%.2f ms", h.outputBufferFrames * 1000 / h.sampleRate) : "—")
            LabeledContent("Output converter + driver", value: ms(h.outputLatencyFrames))
            LabeledContent("Pitch shifter (\(engine.perf.engine.name))", value: String(format: "%.2f ms", stats.algorithmLatencyMs))
            LabeledContent("Total round trip") {
                Text(String(format: "%.2f ms", h.ioLatencyMs + stats.algorithmLatencyMs)).bold()
            }
            LabeledContent("DSP load / dropouts",
                           value: String(format: "%.0f%%  /  %u", stats.dspLoad * 100, stats.overloads))
            HStack {
                Spacer()
                Button("Restart Audio") { engine.restart() }
            }
        }
    }

    // MARK: Gain

    private var gainSection: some View {
        Section("Gain") {
            gainRow("Input gain", value: $engine.perf.inputGainDb)
            gainRow("Output level", value: $engine.perf.outputGainDb)
        }
    }

    private func gainRow(_ title: String, value: Binding<Float>) -> some View {
        HStack {
            Text(title)
            Slider(value: value, in: -24...24, step: 0.5)
            Text(String(format: "%+.1f dB", value.wrappedValue)).monospacedDigit().frame(width: 70, alignment: .trailing)
        }
    }

    // MARK: MIDI

    private var midiSection: some View {
        Section {
            Picker("Channel", selection: $engine.perf.midi.channel) {
                Text("Omni").tag(0)
                ForEach(1...16, id: \.self) { Text("\($0)").tag($0) }
            }
            ccRow("Expression pedal CC", value: $engine.perf.midi.expressionCC)
            ccRow("Footswitch CC (≥64 pressed)", value: $engine.perf.midi.pedalCC)
            ccRow("Setup A CC", value: setupCC(0))
            ccRow("Setup B CC", value: setupCC(1))
            ccRow("Setup C CC", value: setupCC(2))
            Toggle("Program Change 0–2 selects setup A–C", isOn: $engine.perf.midi.programChangeSelectsSetup)
            MIDISourcesRow(midi: engine.midi)
            LabeledContent("Last message", value: engine.lastMIDIEvent)
        } header: {
            Text("MIDI control")
        } footer: {
            Text("Any MIDI expression pedal or footswitch controller works (e.g. via a MIDI-to-USB adapter or a pedal-to-MIDI converter).")
        }
    }

    private struct MIDISourcesRow: View {
        @ObservedObject var midi: MIDIController
        var body: some View {
            LabeledContent("Sources", value: midi.sourceNames.isEmpty ? "none" : midi.sourceNames.joined(separator: ", "))
        }
    }

    private func ccRow(_ title: String, value: Binding<Int>) -> some View {
        Stepper(value: value, in: 0...127) {
            HStack { Text(title); Spacer(); Text("\(value.wrappedValue)").monospacedDigit() }
        }
    }

    private func setupCC(_ i: Int) -> Binding<Int> {
        Binding(get: { engine.perf.midi.setupCCs[i] }, set: { engine.perf.midi.setupCCs[i] = $0 })
    }

    // MARK: Bindings

    private var inputUID: Binding<String?> {
        Binding(get: { engine.currentInput?.uid },
                set: { uid in
                    engine.audio.inputUID = uid
                    if let d = engine.devices.first(where: { $0.uid == uid }), engine.audio.inputChannel >= d.inputChannels {
                        engine.audio.inputChannel = 0
                    }
                })
    }

    private var outputUID: Binding<String?> {
        Binding(get: { engine.currentOutput?.uid },
                set: { uid in
                    engine.audio.outputUID = uid
                    if let d = engine.devices.first(where: { $0.uid == uid }), engine.audio.outputChannelL >= d.outputChannels {
                        engine.audio.outputChannelL = 0
                        engine.audio.outputChannelR = d.outputChannels > 1 ? 1 : -1
                    }
                })
    }

    struct ChannelPair: Hashable { let l: Int; let r: Int }

    private func outputPairs(_ d: AudioDevice) -> [ChannelPair] {
        var pairs: [ChannelPair] = []
        var c = 0
        while c + 1 < d.outputChannels { pairs.append(ChannelPair(l: c, r: c + 1)); c += 2 }
        for m in 0..<d.outputChannels { pairs.append(ChannelPair(l: m, r: -1)) }
        return pairs
    }

    private var outputPair: Binding<ChannelPair> {
        Binding(get: {
                    let r = engine.audio.outputChannelR
                    return ChannelPair(l: engine.audio.outputChannelL, r: r == engine.audio.outputChannelL + 1 ? r : -1)
                },
                set: { p in
                    engine.audio.outputChannelL = p.l
                    engine.audio.outputChannelR = p.r
                })
    }
}
