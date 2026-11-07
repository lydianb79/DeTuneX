// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "DeTuneX",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "DeTuneX", targets: ["DeTuneX"])
    ],
    targets: [
        // Portable realtime DSP (C++17, C interface). Also built by `make test`.
        .target(
            name: "DSPCore",
            path: "Sources/DSPCore",
            cxxSettings: [
                .unsafeFlags(["-O3", "-ffast-math", "-fno-finite-math-only"])
            ]
        ),
        // CoreAudio HAL duplex host (C++). The audio callback never touches Swift.
        .target(
            name: "AudioHost",
            dependencies: ["DSPCore"],
            path: "Sources/AudioHost",
            cxxSettings: [.unsafeFlags(["-O3"])],
            linkerSettings: [
                .linkedFramework("CoreAudio"),
                .linkedFramework("AudioToolbox"),
                .linkedFramework("CoreFoundation")
            ]
        ),
        // SwiftUI app + Metal visualisation + CoreMIDI control.
        .executableTarget(
            name: "DeTuneX",
            dependencies: ["DSPCore", "AudioHost"],
            path: "Sources/DeTuneX",
            linkerSettings: [
                .linkedFramework("Metal"),
                .linkedFramework("MetalKit"),
                .linkedFramework("CoreMIDI"),
                .linkedFramework("AVFoundation")
            ]
        )
    ],
    cxxLanguageStandard: .cxx17
)
