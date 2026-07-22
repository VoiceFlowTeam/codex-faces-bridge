// swift-tools-version: 5.10

import PackageDescription

let package = Package(
    name: "CodexMicroSoftwareBridge",
    platforms: [.macOS(.v13)],
    products: [
        .library(name: "CodexMicroBridgeCore", targets: ["CodexMicroBridgeCore"]),
        .executable(name: "codex-micro-software-bridge", targets: ["CodexMicroSoftwareBridge"]),
    ],
    targets: [
        .target(name: "CodexMicroBridgeCore"),
        .executableTarget(
            name: "CodexMicroSoftwareBridge",
            dependencies: ["CodexMicroBridgeCore"],
            linkerSettings: [
                .linkedFramework("IOKit"),
                .linkedFramework("CoreBluetooth"),
            ]
        ),
        .testTarget(
            name: "CodexMicroBridgeCoreTests",
            dependencies: ["CodexMicroBridgeCore"]
        ),
    ]
)
