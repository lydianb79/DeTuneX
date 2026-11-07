import simd
import SwiftUI

/// A complete look for the app: pedal enclosure, window, panels and the Metal scope.
struct Theme: Identifiable, Equatable {
    let id: String
    let name: String
    let colorScheme: ColorScheme

    // Pedal enclosure
    let pedalTop: Color
    let pedalBottom: Color
    let onPedal: Color          // silk-screen text / tick labels
    let plate: Color            // label plate behind the name
    let led: Color

    // Window
    let windowBackground: Color
    let panel: Color            // translucent panel fill
    let accent: Color

    // Metal scope (linear 0…1 RGB)
    let scopeBase: SIMD3<Float>
    let scopeGlow: SIMD3<Float>
    let traceWet: SIMD3<Float>
    let traceDry: SIMD3<Float>

    var accentRGBA: SIMD4<Float> { SIMD4(traceWet, 1) }

    static func == (a: Theme, b: Theme) -> Bool { a.id == b.id }
}

extension Theme {
    private static func c(_ r: Double, _ g: Double, _ b: Double) -> Color { Color(red: r, green: g, blue: b) }

    static let ocean = Theme(
        id: "ocean", name: "Ocean", colorScheme: .dark,
        pedalTop: c(0.07, 0.28, 0.42), pedalBottom: c(0.03, 0.15, 0.25),
        onPedal: .white, plate: Color.black.opacity(0.25), led: .red,
        windowBackground: c(0.06, 0.065, 0.08), panel: Color.white.opacity(0.04), accent: c(0.33, 0.78, 1.0),
        scopeBase: SIMD3(0.035, 0.04, 0.055), scopeGlow: SIMD3(0.10, 0.30, 0.45),
        traceWet: SIMD3(0.33, 0.78, 1.0), traceDry: SIMD3(0.55, 0.58, 0.66))

    static let neon = Theme(
        id: "neon", name: "Neon", colorScheme: .dark,
        pedalTop: c(0.25, 0.09, 0.40), pedalBottom: c(0.09, 0.03, 0.18),
        onPedal: .white, plate: Color.black.opacity(0.3), led: c(1.0, 0.3, 0.8),
        windowBackground: c(0.045, 0.035, 0.07), panel: Color.white.opacity(0.045), accent: c(1.0, 0.38, 0.84),
        scopeBase: SIMD3(0.035, 0.025, 0.06), scopeGlow: SIMD3(0.45, 0.12, 0.45),
        traceWet: SIMD3(1.0, 0.38, 0.84), traceDry: SIMD3(0.35, 0.80, 1.0))

    static let crimson = Theme(
        id: "crimson", name: "Crimson", colorScheme: .dark,
        pedalTop: c(0.62, 0.09, 0.11), pedalBottom: c(0.32, 0.03, 0.05),
        onPedal: .white, plate: Color.black.opacity(0.28), led: c(1.0, 0.75, 0.2),
        windowBackground: c(0.07, 0.05, 0.05), panel: Color.white.opacity(0.045), accent: c(1.0, 0.58, 0.22),
        scopeBase: SIMD3(0.05, 0.03, 0.03), scopeGlow: SIMD3(0.45, 0.18, 0.06),
        traceWet: SIMD3(1.0, 0.58, 0.22), traceDry: SIMD3(0.66, 0.55, 0.52))

    static let emerald = Theme(
        id: "emerald", name: "Emerald", colorScheme: .dark,
        pedalTop: c(0.05, 0.40, 0.27), pedalBottom: c(0.02, 0.19, 0.13),
        onPedal: .white, plate: Color.black.opacity(0.25), led: .red,
        windowBackground: c(0.04, 0.065, 0.055), panel: Color.white.opacity(0.04), accent: c(0.50, 0.98, 0.50),
        scopeBase: SIMD3(0.025, 0.05, 0.04), scopeGlow: SIMD3(0.10, 0.40, 0.20),
        traceWet: SIMD3(0.50, 0.98, 0.50), traceDry: SIMD3(0.55, 0.66, 0.60))

    static let stealth = Theme(
        id: "stealth", name: "Stealth", colorScheme: .dark,
        pedalTop: c(0.22, 0.22, 0.24), pedalBottom: c(0.08, 0.08, 0.09),
        onPedal: c(0.88, 0.88, 0.9), plate: Color.black.opacity(0.35), led: c(0.3, 0.75, 1.0),
        windowBackground: c(0.035, 0.035, 0.04), panel: Color.white.opacity(0.035), accent: c(0.92, 0.93, 0.96),
        scopeBase: SIMD3(0.02, 0.02, 0.025), scopeGlow: SIMD3(0.22, 0.22, 0.26),
        traceWet: SIMD3(0.95, 0.96, 1.0), traceDry: SIMD3(0.45, 0.45, 0.5))

    static let vintage = Theme(
        id: "vintage", name: "Vintage", colorScheme: .light,
        pedalTop: c(0.95, 0.90, 0.78), pedalBottom: c(0.83, 0.76, 0.60),
        onPedal: c(0.22, 0.15, 0.08), plate: c(0.36, 0.22, 0.12).opacity(0.9), led: .red,
        windowBackground: c(0.93, 0.91, 0.87), panel: Color.black.opacity(0.045), accent: c(0.80, 0.42, 0.10),
        scopeBase: SIMD3(0.10, 0.08, 0.06), scopeGlow: SIMD3(0.45, 0.28, 0.08),
        traceWet: SIMD3(1.0, 0.72, 0.28), traceDry: SIMD3(0.60, 0.55, 0.48))

    static let all: [Theme] = [.stealth, .ocean, .neon, .crimson, .emerald, .vintage]

    static func named(_ id: String) -> Theme { all.first { $0.id == id } ?? .stealth }
}

private struct ThemeKey: EnvironmentKey { static let defaultValue: Theme = .stealth }

extension EnvironmentValues {
    var theme: Theme {
        get { self[ThemeKey.self] }
        set { self[ThemeKey.self] = newValue }
    }
}

/// Small swatch used in the theme menus.
struct ThemeSwatch: View {
    let theme: Theme
    var body: some View {
        HStack(spacing: 0) {
            theme.pedalTop
            theme.accent
        }
        .frame(width: 22, height: 12)
        .clipShape(RoundedRectangle(cornerRadius: 3))
    }
}
