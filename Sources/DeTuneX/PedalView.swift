import SwiftUI

/// The compact-pedal front panel: CHECK LED, BALANCE and SHIFT knobs,
/// SHIFT DIRECTION and PEDAL MODE switches, and the footswitch.
struct PedalView: View {
    @EnvironmentObject var engine: AudioEngine
    @EnvironmentObject var stats: LiveStats
    @Environment(\.theme) private var theme

    var body: some View {
        VStack(spacing: 0) {
            // --- Control section ---
            VStack(spacing: 10) {
                HStack {
                    Spacer()
                    VStack(spacing: 3) {
                        LED(level: Double(stats.wet), color: theme.led)
                        Text("CHECK").font(.system(size: 8, weight: .bold)).foregroundStyle(theme.onPedal.opacity(0.6))
                    }
                    Spacer()
                }
                .padding(.top, 12)

                HStack(alignment: .top, spacing: 0) {
                    VStack(spacing: 0) {
                        Knob(value: balanceBinding, defaultValue: 1, size: 66)
                        Text("BALANCE").font(.system(size: 10, weight: .heavy, design: .rounded)).foregroundStyle(theme.onPedal)
                        Text("\(Int(engine.activeSetup.balance * 100))% FX")
                            .font(.system(size: 9, design: .monospaced)).foregroundStyle(theme.onPedal.opacity(0.6))
                    }
                    .frame(maxWidth: .infinity)
                    VStack(spacing: 0) {
                        Knob(value: shiftBinding, steps: ShiftPosition.allCases.count, defaultValue: 1.0 / 10.0, size: 66,
                             tickLabels: ShiftPosition.allCases.map(\.label))
                        Text("SHIFT").font(.system(size: 10, weight: .heavy, design: .rounded)).foregroundStyle(theme.onPedal)
                        Text(engine.activeSetup.summary)
                            .font(.system(size: 9, design: .monospaced)).foregroundStyle(theme.onPedal.opacity(0.6))
                            .lineLimit(1)
                    }
                    .frame(maxWidth: .infinity)
                }

                HStack {
                    SlideSwitch(title: "SHIFT DIRECTION", left: "DOWN", right: "UP", isRight: directionBinding)
                        .frame(maxWidth: .infinity)
                    SlideSwitch(title: "PEDAL MODE", left: "TOGGLE", right: "MOMENT", isRight: momentaryBinding)
                        .frame(maxWidth: .infinity)
                }
                .padding(.top, 4)

                if engine.activeSetup.shift == .detune {
                    VStack(spacing: 2) {
                        Slider(value: detuneBinding, in: 1...20, step: 1)
                            .controlSize(.small)
                        Text("DETUNE \(Int(engine.activeSetup.detuneCents)) CENT")
                            .font(.system(size: 9, weight: .bold, design: .rounded)).foregroundStyle(theme.onPedal.opacity(0.8))
                    }
                    .padding(.horizontal, 26)
                    .transition(.opacity)
                }
            }
            .padding(.bottom, 14)

            // --- Label plate ---
            VStack(spacing: 2) {
                Text("De-TuneX").font(.system(size: 22, weight: .black, design: .rounded)).foregroundStyle(.white)
                Text("POLY PITCH SHIFTER").font(.system(size: 9, weight: .bold)).tracking(2).foregroundStyle(.white.opacity(0.75))
            }
            .padding(.vertical, 10)
            .frame(maxWidth: .infinity)
            .background(theme.plate)

            // --- Footswitch ---
            Footswitch()
                .padding(.vertical, 18)
        }
        .frame(width: 290)
        .background(
            RoundedRectangle(cornerRadius: 18)
                .fill(LinearGradient(colors: [theme.pedalTop, theme.pedalBottom], startPoint: .top, endPoint: .bottom))
                .shadow(color: .black.opacity(0.6), radius: 18, y: 10)
        )
        .clipShape(RoundedRectangle(cornerRadius: 18))
        .animation(.easeInOut(duration: 0.15), value: engine.activeSetup.shift == .detune)
    }

    // MARK: Bindings into the active setup

    private var balanceBinding: Binding<Double> {
        Binding(get: { Double(engine.activeSetup.balance) },
                set: { engine.activeSetup.balance = Float($0) })
    }
    private var shiftBinding: Binding<Double> {
        let n = Double(ShiftPosition.allCases.count - 1)
        return Binding(get: { Double(engine.activeSetup.shift.rawValue) / n },
                       set: { engine.activeSetup.shift = ShiftPosition(rawValue: Int(($0 * n).rounded())) ?? .st1 })
    }
    private var directionBinding: Binding<Bool> {
        Binding(get: { engine.activeSetup.up }, set: { engine.activeSetup.up = $0 })
    }
    private var momentaryBinding: Binding<Bool> {
        Binding(get: { engine.perf.pedalMode == .momentary }, set: { engine.perf.pedalMode = $0 ? .momentary : .toggle })
    }
    private var detuneBinding: Binding<Float> {
        Binding(get: { engine.activeSetup.detuneCents }, set: { engine.activeSetup.detuneCents = $0 })
    }
}

/// Press/hold aware footswitch (momentary mode needs the release).
struct Footswitch: View {
    @EnvironmentObject var engine: AudioEngine
    @Environment(\.theme) private var theme
    @State private var touching = false

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 14)
                .fill(LinearGradient(colors: [Color(white: 0.16), Color(white: 0.05)], startPoint: .top, endPoint: .bottom))
                .frame(width: 210, height: 96)
                .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.black.opacity(0.8), lineWidth: 2))
            VStack(spacing: 4) {
                Image(systemName: engine.engaged ? "power.circle.fill" : "power.circle")
                    .font(.system(size: 22))
                    .foregroundStyle(engine.engaged ? theme.led : Color.white.opacity(0.5))
                Text(engine.perf.pedalMode == .momentary ? "HOLD" : "ON / OFF")
                    .font(.system(size: 10, weight: .bold, design: .rounded))
                    .foregroundStyle(.white.opacity(0.6))
                Text("space bar").font(.system(size: 8)).foregroundStyle(.white.opacity(0.35))
            }
        }
        .scaleEffect(engine.pedalDown ? 0.97 : 1)
        .animation(.easeOut(duration: 0.05), value: engine.pedalDown)
        .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { _ in
                    if !touching { touching = true; engine.pedalPressed() }
                }
                .onEnded { _ in
                    touching = false
                    engine.pedalReleased()
                }
        )
        .accessibilityLabel("Footswitch")
        .accessibilityAddTraits(.isButton)
    }
}
