import Foundation

/// The XS-1 SHIFT knob: DETUNE, 1–7 semitones, 1–3 octaves. Direction comes from the UP/DOWN switch.
enum ShiftPosition: Int, CaseIterable, Codable, Identifiable {
    case detune = 0, st1, st2, st3, st4, st5, st6, st7, oct1, oct2, oct3
    var id: Int { rawValue }

    var label: String {
        switch self {
        case .detune: return "DT"
        case .oct1: return "1oct"
        case .oct2: return "2oct"
        case .oct3: return "3oct"
        default: return "\(rawValue)"
        }
    }

    var longLabel: String {
        switch self {
        case .detune: return "Detune"
        case .st1: return "1 semitone"
        case .oct1: return "1 octave"
        case .oct2: return "2 octaves"
        case .oct3: return "3 octaves"
        default: return "\(rawValue) semitones"
        }
    }

    /// Magnitude in semitones (detune handled separately).
    var semitones: Float {
        switch self {
        case .detune: return 0
        case .oct1: return 12
        case .oct2: return 24
        case .oct3: return 36
        default: return Float(rawValue)
        }
    }
}

enum PedalMode: String, Codable, CaseIterable { case toggle, momentary }

enum Engine: Int, Codable, CaseIterable, Identifiable {
    case tight = 0, balanced, wide, studio
    var id: Int { rawValue }
    var name: String {
        switch self {
        case .tight: return "Tight"
        case .balanced: return "Balanced"
        case .wide: return "Wide"
        case .studio: return "Studio"
        }
    }
    var help: String {
        switch self {
        case .tight: return "Lowest latency (~1–6 ms). Best for detune and small intervals above the low strings."
        case .balanced: return "Default. Clean chords across the neck, ~3–8 ms."
        case .wide: return "Longest windows for bass, drop tunings and octave-down chords, ~4–14 ms."
        case .studio: return "Phase-locked vocoder. Most transparent mid/high chords, but ~43 ms latency — practice and recording, not live feel."
        }
    }
}

/// One tuning setup — what the XS-1 lets you recall from external footswitches.
struct TuningSetup: Codable, Equatable {
    var shift: ShiftPosition = .st1
    var up: Bool = false              // SHIFT DIRECTION switch
    var detuneCents: Float = 10       // used when shift == .detune (1…20)
    var balance: Float = 1.0          // 0 direct … 1 effect

    var semitones: Float {
        let sign: Float = up ? 1 : -1
        if shift == .detune { return sign * detuneCents / 100 }
        return sign * shift.semitones
    }

    var summary: String {
        if shift == .detune { return String(format: "Detune %@%.0f¢", up ? "+" : "−", detuneCents) }
        return "\(shift.longLabel) \(up ? "up" : "down")"
    }

    static let defaults: [TuningSetup] = [
        TuningSetup(shift: .st1, up: false, balance: 1.0),            // half step down
        TuningSetup(shift: .st2, up: false, balance: 1.0),            // whole step down / D standard
        TuningSetup(shift: .oct1, up: true, balance: 0.5)             // octave up blended (12-string-ish)
    ]
}

/// Persisted audio configuration.
struct AudioSettings: Codable, Equatable {
    var inputUID: String? = nil
    var outputUID: String? = nil
    var inputChannel: Int = 0
    var outputChannelL: Int = 0
    var outputChannelR: Int = 1        // -1 = mono out
    var sampleRate: Double = 48000
    var bufferFrames: Int = 64
    var ioCycleUsage: Double = 1.0     // 1.0 = safe default; lower = less output latency

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let d = AudioSettings()
        inputUID = try c.decodeIfPresent(String.self, forKey: .inputUID) ?? d.inputUID
        outputUID = try c.decodeIfPresent(String.self, forKey: .outputUID) ?? d.outputUID
        inputChannel = try c.decodeIfPresent(Int.self, forKey: .inputChannel) ?? d.inputChannel
        outputChannelL = try c.decodeIfPresent(Int.self, forKey: .outputChannelL) ?? d.outputChannelL
        outputChannelR = try c.decodeIfPresent(Int.self, forKey: .outputChannelR) ?? d.outputChannelR
        sampleRate = try c.decodeIfPresent(Double.self, forKey: .sampleRate) ?? d.sampleRate
        bufferFrames = try c.decodeIfPresent(Int.self, forKey: .bufferFrames) ?? d.bufferFrames
        ioCycleUsage = try c.decodeIfPresent(Double.self, forKey: .ioCycleUsage) ?? d.ioCycleUsage
    }
}

/// Persisted performance settings.
struct PerformanceSettings: Codable, Equatable {
    var setups: [TuningSetup] = TuningSetup.defaults
    var activeSetup: Int = 0
    var pedalMode: PedalMode = .toggle
    var riseMs: Float = 0
    var fallMs: Float = 0
    var engine: Engine = .balanced
    var expressionActive: Bool = false
    var inputGainDb: Float = 0
    var outputGainDb: Float = 0
    var midi = MIDIMapping()
}

struct MIDIMapping: Codable, Equatable {
    var channel: Int = 0               // 0 = omni, 1…16
    var expressionCC: Int = 11
    var pedalCC: Int = 80              // value ≥ 64 = pressed, < 64 = released
    var setupCCs: [Int] = [81, 82, 83] // momentary press selects setup A/B/C
    var programChangeSelectsSetup: Bool = true
}

enum Store {
    private static let audioKey = "audioSettings.v1"
    private static let perfKey = "performanceSettings.v1"

    static func loadAudio() -> AudioSettings { load(audioKey) ?? AudioSettings() }
    static func save(_ s: AudioSettings) { save(s, audioKey) }
    static func loadPerformance() -> PerformanceSettings {
        var p: PerformanceSettings = load(perfKey) ?? PerformanceSettings()
        if p.setups.count != 3 { p.setups = TuningSetup.defaults }
        p.activeSetup = min(max(p.activeSetup, 0), 2)
        return p
    }
    static func save(_ s: PerformanceSettings) { save(s, perfKey) }

    private static func load<T: Decodable>(_ key: String) -> T? {
        guard let data = UserDefaults.standard.data(forKey: key) else { return nil }
        return try? JSONDecoder().decode(T.self, from: data)
    }
    private static func save<T: Encodable>(_ value: T, _ key: String) {
        if let data = try? JSONEncoder().encode(value) { UserDefaults.standard.set(data, forKey: key) }
    }
}
