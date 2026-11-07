import SwiftUI

struct ContentView: View {
    @EnvironmentObject var engine: AudioEngine
    @Environment(\.theme) private var theme
    @AppStorage("themeID") private var themeID = Theme.stealth.id
    @State private var showAudioSetup = false

    var body: some View {
        HStack(alignment: .top, spacing: 20) {
            PedalView()
            VStack(spacing: 14) {
                ScopePanel()
                SetupsPanel()
                PerformancePanel()
                Spacer(minLength: 0)
            }
            .frame(minWidth: 520)
        }
        .padding(20)
        .frame(minWidth: 880, minHeight: 640)
        .background(theme.windowBackground)
        .preferredColorScheme(theme.colorScheme)
        .tint(theme.accent)
        .animation(.easeInOut(duration: 0.25), value: theme.id)
        .toolbar {
            ToolbarItemGroup {
                StatusBadge()
                Menu {
                    Picker("Theme", selection: $themeID) {
                        ForEach(Theme.all) { t in
                            Text(t.name).tag(t.id)
                        }
                    }
                    .pickerStyle(.inline)
                } label: {
                    Label("Theme", systemImage: "paintpalette")
                }
                .help("Change the look of the pedal and the scope")
                Button { engine.restart() } label: { Label("Restart Audio", systemImage: "arrow.clockwise") }
                    .help("Restart the audio engine")
                Button { showAudioSetup = true } label: { Label("Audio Setup", systemImage: "slider.horizontal.3") }
                    .help("Audio devices, channels, sample rate, buffer size, MIDI")
            }
        }
        .sheet(isPresented: $showAudioSetup) {
            AudioSetupView().environmentObject(engine).environmentObject(engine.stats)
                .environment(\.theme, theme)
                .preferredColorScheme(theme.colorScheme)
                .tint(theme.accent)
        }
        .overlay(alignment: .bottom) {
            if let msg = engine.errorMessage {
                HStack {
                    Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.yellow)
                    Text(msg).font(.callout)
                    Spacer()
                    Button("Audio Setup…") { showAudioSetup = true }
                }
                .padding(12)
                .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 10))
                .padding(16)
            }
        }
        .background(KeyboardShortcuts())
    }
}

// MARK: - Scope + readouts

struct ScopePanel: View {
    @EnvironmentObject var engine: AudioEngine
    @EnvironmentObject var stats: LiveStats
    @Environment(\.theme) private var theme

    var body: some View {
        HStack(spacing: 12) {
            ZStack(alignment: .topLeading) {
                MetalScopeView(processor: engine.processor, theme: theme)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
                VStack(alignment: .leading, spacing: 2) {
                    Text("INPUT").font(.system(size: 9, weight: .bold)).foregroundStyle(.white.opacity(0.45))
                    Spacer()
                    Text("OUTPUT").font(.system(size: 9, weight: .bold)).foregroundStyle(.white.opacity(0.45))
                }
                .padding(8)
            }
            .frame(height: 190)

            VStack(alignment: .trailing, spacing: 6) {
                Text(shiftText)
                    .font(.system(size: 34, weight: .bold, design: .rounded).monospacedDigit())
                    .foregroundStyle(stats.wet > 0.5 ? theme.accent : .secondary)
                Text("semitones").font(.caption2).foregroundStyle(.secondary)
                Spacer()
                HStack(alignment: .bottom, spacing: 8) {
                    LevelMeter(db: stats.inputLevel, clip: stats.inputClip, label: "IN")
                    LevelMeter(db: stats.outputLevel, clip: stats.outputClip, label: "OUT")
                }
                .frame(height: 100)
            }
            .frame(width: 120)
        }
        .padding(12)
        .background(theme.panel, in: RoundedRectangle(cornerRadius: 14))
    }

    private var shiftText: String {
        let s = stats.currentSemitones
        if abs(s) < 0.005 { return "0.00" }
        return String(format: "%+.2f", s)
    }
}

// MARK: - Tuning setups (external footswitch slots)

struct SetupsPanel: View {
    @EnvironmentObject var engine: AudioEngine
    @Environment(\.theme) private var theme

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("TUNING SETUPS").font(.system(size: 10, weight: .bold)).foregroundStyle(.secondary)
            HStack(spacing: 10) {
                ForEach(0..<3, id: \.self) { i in
                    let s = engine.perf.setups[i]
                    let active = engine.perf.activeSetup == i
                    Button {
                        engine.selectSetup(i)
                    } label: {
                        VStack(alignment: .leading, spacing: 3) {
                            HStack {
                                Text(["A", "B", "C"][i]).font(.headline)
                                Spacer()
                                Text("key \(i + 1)").font(.caption2).foregroundStyle(.secondary)
                            }
                            Text(s.summary).font(.callout).lineLimit(1)
                            Text("Balance \(Int(s.balance * 100))%").font(.caption2).foregroundStyle(.secondary)
                        }
                        .padding(10)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(active ? theme.accent.opacity(0.25) : theme.panel,
                                    in: RoundedRectangle(cornerRadius: 10))
                        .overlay(RoundedRectangle(cornerRadius: 10).stroke(active ? theme.accent : .clear, lineWidth: 1.5))
                    }
                    .buttonStyle(.plain)
                }
            }
            Text("The pedal controls edit the selected setup. Recall setups with keys 1–3, MIDI CC or Program Change (like the XS-1's external footswitches).")
                .font(.caption2).foregroundStyle(.tertiary)
        }
        .padding(12)
        .background(theme.panel, in: RoundedRectangle(cornerRadius: 14))
    }
}

// MARK: - Expression, glide, engine

struct PerformancePanel: View {
    @EnvironmentObject var engine: AudioEngine
    @EnvironmentObject var stats: LiveStats
    @Environment(\.theme) private var theme

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Toggle("Expression pedal", isOn: $engine.perf.expressionActive)
                    .toggleStyle(.switch).controlSize(.small)
                Spacer()
                Text(engine.perf.expressionActive ? "heel = 0, toe = full shift" : "MIDI CC \(engine.perf.midi.expressionCC) or slider")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            HStack {
                Image(systemName: "shoeprints.fill").foregroundStyle(.secondary)
                Slider(value: $engine.expression, in: 0...1)
                    .disabled(!engine.perf.expressionActive)
                Text("\(Int(engine.expression * 100))%").font(.caption.monospacedDigit()).frame(width: 40, alignment: .trailing)
            }
            Divider()
            HStack(spacing: 18) {
                labeledSlider("Rise", value: $engine.perf.riseMs)
                labeledSlider("Fall", value: $engine.perf.fallMs)
            }
            Divider()
            HStack {
                Picker("Engine", selection: $engine.perf.engine) {
                    ForEach(Engine.allCases) { Text($0.name).tag($0) }
                }
                .pickerStyle(.segmented)
                .frame(maxWidth: 360)
                Spacer()
                LatencySummary()
            }
            Text(engine.perf.engine.help).font(.caption2).foregroundStyle(.secondary)
        }
        .padding(12)
        .background(theme.panel, in: RoundedRectangle(cornerRadius: 14))
    }

    private func labeledSlider(_ title: String, value: Binding<Float>) -> some View {
        HStack {
            Text(title).font(.caption).frame(width: 30, alignment: .leading)
            Slider(value: value, in: 0...2000)
            Text(value.wrappedValue < 1 ? "off" : "\(Int(value.wrappedValue)) ms")
                .font(.caption.monospacedDigit()).frame(width: 56, alignment: .trailing)
        }
        .help("Glide time of the pitch when the effect is switched \(title == "Rise" ? "on" : "off")")
    }
}

struct LatencySummary: View {
    @EnvironmentObject var engine: AudioEngine
    @EnvironmentObject var stats: LiveStats

    var body: some View {
        let io = engine.hostInfo.ioLatencyMs
        let total = io + stats.algorithmLatencyMs
        VStack(alignment: .trailing, spacing: 1) {
            Text(String(format: "≈ %.1f ms total", total)).font(.callout.monospacedDigit().bold())
                .foregroundStyle(total < 12 ? Color.green : (total < 20 ? Color.yellow : Color.orange))
            Text(String(format: "I/O %.1f + shift %.1f ms", io, stats.algorithmLatencyMs))
                .font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
        }
        .help("Round trip: converters + driver + 2 × buffer, plus the pitch shifter's own delay")
    }
}

struct StatusBadge: View {
    @EnvironmentObject var engine: AudioEngine
    @EnvironmentObject var stats: LiveStats

    var body: some View {
        HStack(spacing: 6) {
            Circle().fill(engine.isRunning ? Color.green : Color.red).frame(width: 8, height: 8)
            if engine.isRunning {
                Text(String(format: "%.0f kHz · %ld smp · DSP %.0f%%", engine.hostInfo.sampleRate / 1000,
                            engine.hostInfo.bufferFrames, stats.dspLoad * 100))
                    .font(.caption.monospacedDigit())
                if stats.overloads > 0 {
                    Text("\(stats.overloads) dropouts").font(.caption).foregroundStyle(.orange)
                }
            } else {
                Text("Audio stopped").font(.caption)
            }
        }
        .padding(.horizontal, 8)
    }
}

// MARK: - Keyboard: space = footswitch (press/release), 1–3 = setups

struct KeyboardShortcuts: NSViewRepresentable {
    @EnvironmentObject var engine: AudioEngine

    func makeNSView(context: Context) -> NSView {
        context.coordinator.install()
        return NSView()
    }
    func updateNSView(_ nsView: NSView, context: Context) { context.coordinator.engine = engine }
    func makeCoordinator() -> Coordinator { Coordinator(engine: engine) }
    static func dismantleNSView(_ nsView: NSView, coordinator: Coordinator) { coordinator.uninstall() }

    @MainActor
    final class Coordinator {
        var engine: AudioEngine
        private var monitor: Any?
        init(engine: AudioEngine) { self.engine = engine }

        func uninstall() {
            if let m = monitor { NSEvent.removeMonitor(m) }
            monitor = nil
        }

        func install() {
            guard monitor == nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp]) { [weak self] event in
                guard let self else { return event }
                // Don't steal keys from text fields (MIDI CC editors etc.).
                if let responder = NSApp.keyWindow?.firstResponder, responder is NSTextView { return event }
                if NSApp.keyWindow?.attachedSheet != nil || NSApp.keyWindow?.isSheet == true { return event }
                switch (event.type, event.keyCode) {
                case (.keyDown, 49): // space
                    if !event.isARepeat { self.engine.pedalPressed() }
                    return nil
                case (.keyUp, 49):
                    self.engine.pedalReleased()
                    return nil
                case (.keyDown, _):
                    if event.modifierFlags.intersection([.command, .option, .control]).isEmpty,
                       let ch = event.charactersIgnoringModifiers, let n = Int(ch), (1...3).contains(n) {
                        self.engine.selectSetup(n - 1)
                        return nil
                    }
                    return event
                default:
                    return event
                }
            }
        }
    }
}
