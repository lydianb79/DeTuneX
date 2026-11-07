import AppKit
import SwiftUI

@main
struct DeTuneXApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var engine = AudioEngine()
    @AppStorage("themeID") private var themeID = Theme.stealth.id

    var body: some Scene {
        Window("De-TuneX", id: "main") {
            ContentView()
                .environmentObject(engine)
                .environmentObject(engine.stats)
                .environment(\.theme, Theme.named(themeID))
        }
        .windowResizability(.contentMinSize)
        .commands {
            CommandMenu("Pedal") {
                Button("Setup A") { engine.selectSetup(0) }
                Button("Setup B") { engine.selectSetup(1) }
                Button("Setup C") { engine.selectSetup(2) }
                Divider()
                Picker("Engine", selection: $engine.perf.engine) {
                    ForEach(Engine.allCases) { Text($0.name).tag($0) }
                }
                Divider()
                Picker("Theme", selection: $themeID) {
                    ForEach(Theme.all) { Text($0.name).tag($0.id) }
                }
                Divider()
                Button("Restart Audio") { engine.restart() }
                    .keyboardShortcut("r", modifiers: [.command, .shift])
            }
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        // Needed when launched from `swift run` (no bundle): become a regular foreground app.
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}
