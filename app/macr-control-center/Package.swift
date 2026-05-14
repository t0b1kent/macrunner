// swift-tools-version:6.0
import PackageDescription

let package = Package(
    name: "MacRunnerControlCenter",
    defaultLocalization: "en",
    platforms: [.macOS(.v14)],
    products: [
        .executable(
            name: "MacRunnerControlCenter",
            targets: ["MacRunnerControlCenter"]
        ),
        .executable(
            name: "macr-hud",
            targets: ["macr-hud"]
        )
    ],
    targets: [
        .executableTarget(
            name: "MacRunnerControlCenter",
            dependencies: [
                "Sparkle",
                "CrashReporter"
            ],
            resources: [.process("Resources")],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .executableTarget(
            name: "macr-hud",
            dependencies: [],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "MacRunnerControlCenterTests",
            dependencies: ["MacRunnerControlCenter"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .binaryTarget(
            name: "Sparkle",
            path: "Frameworks/Sparkle.xcframework"
        ),
        .binaryTarget(
            name: "CrashReporter",
            path: "Frameworks/CrashReporter.xcframework"
        )
    ]
)
