import CoreMIDI
import Foundation

/// Decoded MIDI 1.0 channel messages we care about. Channels are 1…16.
enum MIDIEvent: CustomStringConvertible, Sendable {
    case controlChange(channel: Int, cc: Int, value: Int)
    case programChange(channel: Int, program: Int)

    var channel: Int {
        switch self {
        case let .controlChange(ch, _, _), let .programChange(ch, _): return ch
        }
    }

    var description: String {
        switch self {
        case let .controlChange(ch, cc, v): return "Ch \(ch)  CC \(cc) = \(v)"
        case let .programChange(ch, p): return "Ch \(ch)  PC \(p)"
        }
    }
}

/// Listens to every MIDI source (expression pedals, MIDI footswitches,
/// controllers) and reconnects automatically when devices come and go.
final class MIDIController: ObservableObject, @unchecked Sendable {
    /// Called on the CoreMIDI receive thread.
    var onEvent: (@Sendable (MIDIEvent) -> Void)?

    private var client = MIDIClientRef()
    private var port = MIDIPortRef()
    private var connected: [MIDIEndpointRef] = []
    @Published private(set) var sourceNames: [String] = []   // updated on main

    func start() {
        guard client == 0 else { return }
        let status = MIDIClientCreateWithBlock("De-TuneX" as CFString, &client) { [weak self] notification in
            if notification.pointee.messageID == .msgSetupChanged {
                DispatchQueue.main.async { self?.connectAll() }
            }
        }
        guard status == noErr else { return }
        let portStatus = MIDIInputPortCreateWithProtocol(client, "De-TuneX Input" as CFString, ._1_0, &port) { [weak self] list, _ in
            self?.receive(list)
        }
        guard portStatus == noErr else { return }
        DispatchQueue.main.async { self.connectAll() }
    }

    private func connectAll() {
        for src in connected { MIDIPortDisconnectSource(port, src) }
        connected.removeAll()
        var names: [String] = []
        for i in 0..<MIDIGetNumberOfSources() {
            let src = MIDIGetSource(i)
            guard src != 0, MIDIPortConnectSource(port, src, nil) == noErr else { continue }
            connected.append(src)
            var name: Unmanaged<CFString>?
            if MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &name) == noErr, let n = name?.takeRetainedValue() {
                names.append(n as String)
            }
        }
        sourceNames = names
    }

    private func receive(_ list: UnsafePointer<MIDIEventList>) {
        guard let handler = onEvent else { return }
        let wordsOffset = MemoryLayout<MIDIEventPacket>.offset(of: \MIDIEventPacket.words) ?? 12
        for packet in list.unsafeSequence() {
            let count = Int(packet.pointee.wordCount)
            let words = UnsafeRawPointer(packet).advanced(by: wordsOffset).assumingMemoryBound(to: UInt32.self)
            var i = 0
            while i < count {
                let w = words[i]
                let type = w >> 28
                if type == 0x2 { // MIDI 1.0 channel voice message (one word)
                    let status = (w >> 16) & 0xFF
                    let d1 = Int((w >> 8) & 0x7F)
                    let d2 = Int(w & 0x7F)
                    let ch = Int(status & 0x0F) + 1
                    switch status & 0xF0 {
                    case 0xB0: handler(.controlChange(channel: ch, cc: d1, value: d2))
                    case 0xC0: handler(.programChange(channel: ch, program: d1))
                    default: break
                    }
                }
                i += MIDIController.umpWordCount(type)
            }
        }
    }

    /// Universal MIDI Packet length (in 32-bit words) by message type.
    private static func umpWordCount(_ type: UInt32) -> Int {
        switch type {
        case 0x0, 0x1, 0x2, 0x6, 0x7: return 1
        case 0x3, 0x4, 0x8, 0x9, 0xA: return 2
        case 0xB, 0xC: return 3
        default: return 4
        }
    }
}
