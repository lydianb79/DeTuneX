import SwiftUI

// MARK: - Knob

/// Rotary control: drag up/down (or sideways), scroll wheel friendly via drag,
/// double-click to reset. `steps` makes it a detented selector.
struct Knob: View {
    @Environment(\.theme) private var theme
    @Binding var value: Double          // 0…1
    var steps: Int? = nil
    var defaultValue: Double = 0.5
    var size: CGFloat = 78
    var tickLabels: [String] = []

    @State private var dragStart: Double?

    private var angle: Angle { .degrees(-135 + 270 * value) }

    var body: some View {
        ZStack {
            // Tick ring / labels
            if !tickLabels.isEmpty {
                ForEach(Array(tickLabels.enumerated()), id: \.offset) { i, text in
                    let t = tickLabels.count > 1 ? Double(i) / Double(tickLabels.count - 1) : 0
                    let a = (-135 + 270 * t - 90) * .pi / 180
                    let r = size * 0.5 + 15
                    Text(text)
                        .font(.system(size: 9, weight: .semibold, design: .rounded))
                        .foregroundStyle(abs(t - value) < 0.001 ? theme.onPedal : theme.onPedal.opacity(0.45))
                        .position(x: size / 2 + 20 + CGFloat(cos(a)) * r, y: size / 2 + 20 + CGFloat(sin(a)) * r)
                }
            } else {
                ForEach(0..<11) { i in
                    Capsule()
                        .fill(theme.onPedal.opacity(0.35))
                        .frame(width: 1.5, height: 5)
                        .offset(y: -size / 2 - 7)
                        .rotationEffect(.degrees(-135 + 27 * Double(i)))
                }
                .frame(width: size + 40, height: size + 40)
            }

            Circle()
                .fill(LinearGradient(colors: [Color(white: 0.24), Color(white: 0.07)], startPoint: .top, endPoint: .bottom))
                .overlay(Circle().stroke(Color.black.opacity(0.7), lineWidth: 2))
                .overlay(
                    Circle()
                        .strokeBorder(LinearGradient(colors: [.white.opacity(0.25), .clear], startPoint: .top, endPoint: .bottom), lineWidth: 1.5)
                        .padding(5)
                )
                .shadow(color: .black.opacity(0.6), radius: 5, y: 4)
                .frame(width: size, height: size)

            Capsule()
                .fill(Color.white)
                .frame(width: 3, height: size * 0.3)
                .offset(y: -size * 0.27)
                .rotationEffect(angle)
        }
        .frame(width: size + 40, height: size + 40)
        .contentShape(Circle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { g in
                    if dragStart == nil { dragStart = value }
                    let delta = Double(-g.translation.height / 180 + g.translation.width / 360)
                    var v = min(max((dragStart ?? value) + delta, 0), 1)
                    if let steps, steps > 1 { v = (v * Double(steps - 1)).rounded() / Double(steps - 1) }
                    if v != value { value = v }
                }
                .onEnded { _ in dragStart = nil }
        )
        .simultaneousGesture(TapGesture(count: 2).onEnded { value = defaultValue })
        .accessibilityElement()
        .accessibilityValue(Text("\(Int(value * 100)) percent"))
        .accessibilityAdjustableAction { dir in
            let step = steps.map { 1.0 / Double(max($0 - 1, 1)) } ?? 0.05
            value = min(max(value + (dir == .increment ? step : -step), 0), 1)
        }
    }
}

// MARK: - Two-position slide switch

struct SlideSwitch: View {
    @Environment(\.theme) private var theme
    let title: String
    let left: String
    let right: String
    @Binding var isRight: Bool

    var body: some View {
        VStack(spacing: 5) {
            Text(title).font(.system(size: 9, weight: .bold, design: .rounded)).foregroundStyle(theme.onPedal.opacity(0.7))
            HStack(spacing: 6) {
                Text(left).font(.system(size: 9, weight: .semibold)).foregroundStyle(theme.onPedal.opacity(isRight ? 0.4 : 1))
                ZStack(alignment: isRight ? .trailing : .leading) {
                    Capsule().fill(Color.black.opacity(0.55)).frame(width: 40, height: 18)
                    RoundedRectangle(cornerRadius: 4)
                        .fill(LinearGradient(colors: [Color(white: 0.85), Color(white: 0.55)], startPoint: .top, endPoint: .bottom))
                        .frame(width: 18, height: 14)
                        .padding(.horizontal, 2)
                }
                .animation(.easeOut(duration: 0.12), value: isRight)
                .onTapGesture { isRight.toggle() }
                Text(right).font(.system(size: 9, weight: .semibold)).foregroundStyle(theme.onPedal.opacity(isRight ? 1 : 0.4))
            }
        }
    }
}

// MARK: - LED

struct LED: View {
    var level: Double      // 0…1
    var color: Color = .red

    var body: some View {
        Circle()
            .fill(color.opacity(0.25 + 0.75 * level))
            .frame(width: 12, height: 12)
            .overlay(Circle().stroke(Color.black.opacity(0.6), lineWidth: 1))
            .shadow(color: color.opacity(level), radius: 8 * level)
    }
}

// MARK: - Level meter

struct LevelMeter: View {
    var db: Float
    var clip: Bool
    var label: String

    var body: some View {
        VStack(spacing: 4) {
            GeometryReader { geo in
                let norm = CGFloat(min(max((db + 60) / 60, 0), 1))
                ZStack(alignment: .bottom) {
                    RoundedRectangle(cornerRadius: 3).fill(Color.black.opacity(0.5))
                    RoundedRectangle(cornerRadius: 3)
                        .fill(LinearGradient(colors: [.green, .yellow, .orange, .red], startPoint: .bottom, endPoint: .top))
                        .frame(height: geo.size.height * norm)
                        .mask(alignment: .bottom) { Rectangle().frame(height: geo.size.height * norm) }
                    VStack {
                        Circle().fill(clip ? Color.red : Color.red.opacity(0.15)).frame(width: 7, height: 7).padding(.top, 3)
                        Spacer()
                    }
                }
            }
            .frame(width: 14)
            Text(label).font(.system(size: 9, weight: .semibold)).foregroundStyle(.secondary)
        }
    }
}
