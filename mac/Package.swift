// swift-tools-version:5.9
import PackageDescription

let package = Package(
    name: "StorageScope",
    platforms: [.macOS(.v13)],
    products: [
        .executable(name: "StorageScope", targets: ["StorageScopeMac"])
    ],
    targets: [
        // Everything that is not UI: scanning, search, protection rules, cache discovery, trashing.
        .target(name: "StorageScopeCore"),
        // The SwiftUI app.
        .executableTarget(name: "StorageScopeMac", dependencies: ["StorageScopeCore"]),
        .testTarget(name: "StorageScopeCoreTests", dependencies: ["StorageScopeCore"]),
    ]
)
