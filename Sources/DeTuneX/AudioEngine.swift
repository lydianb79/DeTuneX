import AVFoundation
import AudioHost
import Combine
import CoreAudio
import DSPCore
import Foundation

/// Fast-changing values for meters and readouts (kept separate so the 30 Hz
/// refresh only redraws the views that display them).
@MainActor
final class LiveStats: ObservableObject {
    @Published var inputLevel: Float = 0      // dBFS (peak, with decay)
    @Published var outputLevel: Float = 0
    @Published var inputClip = false
    @Published var outputClip = false
    @Published var currentSemitones: Float = 0
    @Published var wet: Float = 0
    @Published var algorithmLatencyMs: Double = 0
    @Published var dspLoad: Float = 0
    @Published var overloads: UInt32 = 0
}

struct HostInfo: Equatable {
    var sampleRate: Double = 0
    var bufferFrames: Int = 0
    var inputLatencyFrames: Int = 0
    var outputLatencyFrames: Int = 0
    var usingAggregate = false
    var ioCycleUsage: Double = 1

    /// Output-side buffer latency: the HAL calls us `usage` × buffer before the deadline.
    var outputBufferFrames: Double { Double(bufferFrames) * min(max(ioCycleUsage, 0.1), 1) }

    /// Hardware round trip: converter/driver latency + input buffer + (trimmed) output buffer.
    var ioLatencyMs: Double {
        guard sampleRate > 0 else { return 0 }
        return (Double(inputLatencyFrames + outputLatencyFrames + bufferFrames) + outputBufferFrames) * 1000 / sampleRate
    }
    var bufferMs: Double { sampleRate > 0 ? Double(bufferFrames) * 1000 / sampleRate : 0 }
}

@MainActor
final class AudioEngine: ObservableObject {
    let processor: OpaquePointer
    private let host: OpaquePointer
    let stats = LiveStats()
    let midi = MIDIController()

    @Published private(set) var devices: [AudioDevice] = []
    @Published var audio: AudioSettings {
        didSet {
            guard audio != oldValue else { return }
            Store.save(audio)
            scheduleRestart()
        }
    }
    @Published var perf: PerformanceSettings {
        didSet {
            guard perf != oldValue else { return }
            Store.save(perf)
            mappingBox.value = perf.midi
            applyPerformance()
        }
    }
    @Published private(set) var engaged = false
    @Published private(set) var pedalDown = false
    @Published var expression: Float = 1 {
        didSet { dtx_set_param(processor, DTX_PARAM_EXPRESSION, expression) }
    }
    @Published private(set) var isRunning = false
    @Published private(set) var errorMessage: String?
    @Published private(set) var hostInfo = HostInfo()
    @Published private(set) var lastMIDIEvent: String = "—"

    private var restartWork: DispatchWorkItem?
    private var timer: Timer?
    private var activity: NSObjectProtocol?
    private var peakIn: Float = 0, peakOut: Float = 0
    private var clipInHold = 0, clipOutHold = 0

    init() {
        processor = dtx_processor_create()
        host = dtx_host_create(processor)
        audio = Store.loadAudio()
        perf = Store.loadPerformance()
        applyPerformance()
        refreshDevices()
        AudioDevices.observeChanges { [weak self] in
            Task { @MainActor in self?.devicesChanged() }
        }
        setupMIDI()
        // .common mode: keeps meters and device-loss detection alive during slider drags / menus.
        let t = Timer(timeInterval: 1.0 / 30.0, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.tick() }
        }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        requestPermissionAndStart()
    }

    deinit {
        dtx_host_destroy(host)
        dtx_processor_destroy(processor)
    }

    // MARK: - Setup editing (front-panel controls edit the active setup)

    var activeSetup: TuningSetup {
        get { perf.setups[perf.activeSetup] }
        set { perf.setups[perf.activeSetup] = newValue }
    }

    func selectSetup(_ index: Int, engage: Bool = true) {
        guard (0..<perf.setups.count).contains(index) else { return }
        perf.activeSetup = index
        if engage && !engaged { setEngaged(true) }
    }

    // MARK: - Pedal (footswitch) logic — TOGGLE latches, MOMENT is on while held

    func pedalPressed() {
        guard !pedalDown else { return }
        pedalDown = true
        switch perf.pedalMode {
        case .toggle: setEngaged(!engaged)
        case .momentary: setEngaged(true)
        }
    }

    func pedalReleased() {
        guard pedalDown else { return }
        pedalDown = false
        if perf.pedalMode == .momentary { setEngaged(false) }
    }

    private func setEngaged(_ on: Bool) {
        engaged = on
        dtx_set_param(processor, DTX_PARAM_ENGAGED, on ? 1 : 0)
    }

    // MARK: - Parameters → DSP

    private func applyPerformance() {
        let s = perf.setups[perf.activeSetup]
        dtx_set_param(processor, DTX_PARAM_SEMITONES, s.semitones)
        dtx_set_param(processor, DTX_PARAM_BALANCE, s.balance)
        dtx_set_param(processor, DTX_PARAM_RISE_MS, perf.riseMs)
        dtx_set_param(processor, DTX_PARAM_FALL_MS, perf.fallMs)
        dtx_set_param(processor, DTX_PARAM_ENGINE, Float(perf.engine.rawValue))
        dtx_set_param(processor, DTX_PARAM_EXPRESSION_ACTIVE, perf.expressionActive ? 1 : 0)
        dtx_set_param(processor, DTX_PARAM_EXPRESSION, expression)
        dtx_set_param(processor, DTX_PARAM_INPUT_GAIN_DB, perf.inputGainDb)
        dtx_set_param(processor, DTX_PARAM_OUTPUT_GAIN_DB, perf.outputGainDb)
        // Leaving momentary mode while held must not leave the effect stuck on.
        if perf.pedalMode == .momentary && !pedalDown && engaged { setEngaged(false) }
    }

    // MARK: - Devices

    var inputDevices: [AudioDevice] { devices.filter { $0.inputChannels > 0 } }
    var outputDevices: [AudioDevice] { devices.filter { $0.outputChannels > 0 } }

    var currentInput: AudioDevice? {
        devices.first { $0.uid == audio.inputUID && $0.inputChannels > 0 }
            ?? devices.first { $0.id == AudioDevices.defaultDevice(input: true) && $0.inputChannels > 0 }
            ?? inputDevices.first
    }
    var currentOutput: AudioDevice? {
        devices.first { $0.uid == audio.outputUID && $0.outputChannels > 0 }
            ?? devices.first { $0.id == AudioDevices.defaultDevice(input: false) && $0.outputChannels > 0 }
            ?? outputDevices.first
    }

    /// Sample rates both selected devices support.
    var commonSampleRates: [Double] {
        guard let i = currentInput, let o = currentOutput else { return [44100, 48000] }
        let rates = i.id == o.id ? o.sampleRates : o.sampleRates.filter { i.sampleRates.contains($0) }
        return rates.isEmpty ? o.sampleRates : rates
    }

    /// Buffer sizes offered in the UI, limited to what the devices accept.
    var bufferChoices: [Int] {
        let candidates = [16, 32, 48, 64, 96, 128, 192, 256, 512, 1024]
        var range = currentOutput?.bufferRange ?? 16...4096
        if let i = currentInput, i.id != currentOutput?.id {
            let lo = max(range.lowerBound, i.bufferRange.lowerBound)
            let hi = min(range.upperBound, i.bufferRange.upperBound)
            if lo <= hi { range = lo...hi }
        }
        let r = range.lowerBound <= range.upperBound ? range : 16...4096
        let list = candidates.filter { r.contains($0) }
        return list.isEmpty ? [r.lowerBound] : list
    }

    func refreshDevices() { devices = AudioDevices.all() }

    private func devicesChanged() {
        // Our own private aggregate is filtered out of `devices`, so creating or
        // destroying it does not count as a change (avoids a restart loop).
        let beforeIDs = devices.map(\.uid)
        let before = (currentInput?.uid, currentOutput?.uid)
        refreshDevices()
        let listChanged = devices.map(\.uid) != beforeIDs
        if before != (currentInput?.uid, currentOutput?.uid) || (!isRunning && listChanged) { scheduleRestart() }
    }

    // MARK: - Start / stop

    private func requestPermissionAndStart() {
        switch AVCaptureDevice.authorizationStatus(for: .audio) {
        case .authorized:
            scheduleRestart(delay: 0)
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .audio) { @Sendable granted in
                Task { @MainActor in
                    if granted { self.scheduleRestart(delay: 0) }
                    else { self.errorMessage = "Microphone access was denied. Enable De-TuneX in System Settings › Privacy & Security › Microphone." }
                }
            }
        default:
            errorMessage = "Microphone access is off. Enable De-TuneX in System Settings › Privacy & Security › Microphone, then press Restart."
        }
    }

    func scheduleRestart(delay: TimeInterval = 0.15) {
        restartWork?.cancel()
        let work = DispatchWorkItem { [weak self] in Task { @MainActor in self?.restart() } }
        restartWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: work)
    }

    func restart() {
        stop()
        guard AVCaptureDevice.authorizationStatus(for: .audio) == .authorized else {
            requestPermissionAndStart()
            return
        }
        guard let input = currentInput, let output = currentOutput else {
            errorMessage = "No audio input/output device found."
            return
        }
        let inCh = min(max(audio.inputChannel, 0), input.inputChannels - 1)
        let outL = min(max(audio.outputChannelL, 0), output.outputChannels - 1)
        var outR = audio.outputChannelR
        if outR >= output.outputChannels || outR == outL { outR = -1 }
        let rates = commonSampleRates
        let rate = rates.contains(audio.sampleRate) ? audio.sampleRate : (rates.contains(48000) ? 48000 : (rates.last ?? 0))

        var cfg = DTXHostConfig(inputDevice: input.id, outputDevice: output.id,
                                inputChannel: Int32(inCh), outputChannelL: Int32(outL), outputChannelR: Int32(outR),
                                sampleRate: rate, bufferFrames: UInt32(max(audio.bufferFrames, 8)),
                                ioCycleUsage: Float(audio.ioCycleUsage))
        var st = DTXHostStatus()
        var err = [CChar](repeating: 0, count: 512)
        let result = dtx_host_start(host, &cfg, &st, &err, Int32(err.count))
        if result == 0 {
            isRunning = true
            errorMessage = nil
            hostInfo = HostInfo(sampleRate: st.sampleRate, bufferFrames: Int(st.bufferFrames),
                                inputLatencyFrames: Int(st.inputLatencyFrames), outputLatencyFrames: Int(st.outputLatencyFrames),
                                usingAggregate: st.usingAggregate != 0,
                                ioCycleUsage: Double(st.ioCycleUsage))
            if activity == nil {
                activity = ProcessInfo.processInfo.beginActivity(
                    options: [.userInitiated, .latencyCritical, .idleSystemSleepDisabled],
                    reason: "Realtime audio processing")
            }
        } else {
            errorMessage = err.withUnsafeBufferPointer { String(cString: $0.baseAddress!) }
        }
    }

    func stop() {
        dtx_host_stop(host)
        isRunning = false
        if let a = activity { ProcessInfo.processInfo.endActivity(a); activity = nil }
    }

    // MARK: - Metering

    private func tick() {
        guard isRunning else { return }
        if dtx_host_device_lost(host) != 0 {
            stop()                       // stops the tick from re-postponing the restart
            refreshDevices()
            scheduleRestart(delay: 0.3)
            return
        }
        let inPk = dtx_take_meter_peak(processor, DTX_METER_INPUT)
        let outPk = dtx_take_meter_peak(processor, DTX_METER_OUTPUT)
        peakIn = max(inPk, peakIn * 0.85)
        peakOut = max(outPk, peakOut * 0.85)
        if inPk >= 0.99 { clipInHold = 45 } else if clipInHold > 0 { clipInHold -= 1 }
        if outPk >= 0.99 { clipOutHold = 45 } else if clipOutHold > 0 { clipOutHold -= 1 }

        let s = stats
        s.inputLevel = 20 * log10(max(peakIn, 1e-5))
        s.outputLevel = 20 * log10(max(peakOut, 1e-5))
        s.inputClip = clipInHold > 0
        s.outputClip = clipOutHold > 0
        s.currentSemitones = dtx_get_current_semitones(processor)
        s.wet = dtx_get_wet_amount(processor)
        s.algorithmLatencyMs = hostInfo.sampleRate > 0
            ? Double(dtx_get_latency_samples(processor)) * 1000 / hostInfo.sampleRate : 0
        s.dspLoad = dtx_host_dsp_load(host)
        s.overloads = dtx_host_overload_count(host)
    }

    // MARK: - MIDI

    private func setupMIDI() {
        mappingBox.value = perf.midi
        let proc = ProcessorRef(pointer: processor)
        let box = mappingBox
        midi.onEvent = { [weak self] event in
            // Runs on the CoreMIDI thread. Expression goes straight to the DSP
            // (lock-free atomics) for the tightest response; the rest hops to main.
            let map = box.value
            if map.channel != 0 && event.channel != map.channel { return }
            if case let .controlChange(_, cc, value) = event, cc == map.expressionCC {
                dtx_set_param(proc.pointer, DTX_PARAM_EXPRESSION, Float(value) / 127)
            }
            Task { @MainActor in self?.handleMIDI(event) }
        }
        midi.start()
    }

    private let mappingBox = LockedBox(MIDIMapping())

    private func handleMIDI(_ event: MIDIEvent) {
        let map = perf.midi
        lastMIDIEvent = event.description
        if map.channel != 0 && event.channel != map.channel { return }
        switch event {
        case let .controlChange(_, cc, value):
            if cc == map.expressionCC {
                expression = Float(value) / 127
            } else if cc == map.pedalCC {
                value >= 64 ? pedalPressed() : pedalReleased()
            } else if let i = map.setupCCs.firstIndex(of: cc), value >= 64 {
                selectSetup(i)
            }
        case let .programChange(_, program):
            if map.programChangeSelectsSetup && program < perf.setups.count { selectSetup(program) }
        }
    }
}

/// Lets the CoreMIDI thread hold the processor pointer (the C API is thread-safe).
struct ProcessorRef: @unchecked Sendable { let pointer: OpaquePointer }

/// Minimal lock-protected value shared with the MIDI thread.
final class LockedBox<T>: @unchecked Sendable {
    private let lock = NSLock()
    private var _value: T
    init(_ v: T) { _value = v }
    var value: T {
        get { lock.lock(); defer { lock.unlock() }; return _value }
        set { lock.lock(); _value = newValue; lock.unlock() }
    }
}
