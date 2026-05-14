import Foundation

struct PEInspectionService {
    static func inspect(exePath: String, macRunnerRoot: String) async -> PEInspectionResult? {
        let script = "\(macRunnerRoot)/scripts/run-windows-app.sh"
        guard FileManager.default.fileExists(atPath: script) else { return nil }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: script)
        process.arguments = [exePath, "--timeout", "5", "--json", "/dev/null"]
        process.currentDirectoryURL = URL(fileURLWithPath: macRunnerRoot)

        let outPipe = Pipe()
        let errPipe = Pipe()
        process.standardOutput = outPipe
        process.standardError = errPipe

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return nil
        }

        let stdout = String(data: outPipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""
        let stderr = String(data: errPipe.fileHandleForReading.readDataToEndOfFile(), encoding: .utf8) ?? ""

        var arch = "unknown"
        var subsystem = "unknown"
        var d3dDetected = false

        for line in stdout.split(separator: "\n") {
            let text = line.lowercased()
            if text.contains("machine=") {
                let parts = text.split(separator: "=")
                if parts.count > 1 { arch = String(parts[1]).trimmingCharacters(in: .whitespaces) }
            }
            if text.contains("subsystem=") {
                let parts = text.split(separator: "=")
                if parts.count > 1 { subsystem = String(parts[1]).trimmingCharacters(in: .whitespaces) }
            }
        }

        let lowerStderr = stderr.lowercased()
        d3dDetected = lowerStderr.contains("d3d11") || lowerStderr.contains("d3d12") || lowerStderr.contains("dxgi") || lowerStderr.contains("d3d9")

        var result = PEInspectionResult(
            arch: arch,
            subsystem: subsystem,
            d3dDetected: d3dDetected,
            suggestedD3DBackend: d3dDetected ? "mock" : "none",
            suggestedTimeout: subsystem == "gui" ? 120 : 45
        )

        let peInspector = "\(macRunnerRoot)/tools/pe_inspector.py"
        if FileManager.default.fileExists(atPath: peInspector) {
            let pe = Process()
            pe.executableURL = URL(fileURLWithPath: "/usr/bin/python3")
            pe.arguments = [peInspector, exePath, "--json"]
            let peOut = Pipe()
            pe.standardOutput = peOut
            try? pe.run()
            pe.waitUntilExit()
            if let data = try? JSONSerialization.jsonObject(with: peOut.fileHandleForReading.readDataToEndOfFile()) as? [String: Any] {
                if let m = data["machine"] as? String { result.arch = m }
                if let s = data["subsystem"] as? String { result.subsystem = s }
                if let imports = data["imports"] as? [String] {
                    result.imports = imports
                    result.d3dDetected = imports.contains { $0.lowercased().contains("d3d") || $0.lowercased().contains("dxgi") }
                }
                if let exports = data["exports"] as? [String] { result.exports = exports }
                if let sections = data["sections"] as? [String] { result.sections = sections }
                if let deps = data["dll_dependencies"] as? [String] { result.dllDependencies = deps }
                if let dotNet = data["is_dotnet"] as? Bool { result.isDotNet = dotNet }
                if let is64 = data["is_64bit"] as? Bool { result.is64Bit = is64 }
                if let size = data["file_size"] as? Int64 { result.fileSizeBytes = size }
                if let icon = data["icon_path"] as? String { result.iconPath = icon }
            }
        }

        classify(result: &result)
        suggestLane(result: &result)

        if result.d3dDetected {
            result.suggestedD3DBackend = "mock"
        }
        result.suggestedTimeout = result.subsystem == "gui" ? 120 : 45

        return result
    }

    static func scanFolder(_ path: String) -> [String] {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: path, isDirectory: &isDir), isDir.boolValue else {
            return []
        }
        guard let enumerator = fm.enumerator(atPath: path) else { return [] }
        var exes: [String] = []
        for case let file as String in enumerator {
            if file.hasSuffix(".exe") {
                exes.append("\(path)/\(file)")
            }
        }
        return exes
    }

    static func isDuplicate(exePath: String, apps: [AppEntry]) -> Bool {
        let normalized = exePath.lowercased()
        return apps.contains { $0.exePath.lowercased() == normalized }
    }

    static func createManifestEntry(from result: PEInspectionResult, exePath: String, name: String) -> ManifestApp {
        ManifestApp(
            name: name,
            path: exePath,
            arch: result.arch == "unknown" ? nil : result.arch,
            args: nil,
            env: nil,
            workdir: nil,
            expectedRc: nil,
            expectedStdoutContains: nil,
            expectedFiles: nil,
            d3dBackend: result.suggestedD3DBackend,
            allowGui: result.subsystem == "gui" ? true : nil,
            timeout: result.suggestedTimeout
        )
    }

    private static func classify(result: inout PEInspectionResult) {
        let deps = result.dllDependencies ?? []
        let lowerDeps = deps.map { $0.lowercased() }

        result.hasGraphics = lowerDeps.contains(where: { $0.contains("d3d") || $0.contains("dxgi") || $0.contains("opengl") || $0.contains("vulkan") })
        result.hasAudio = lowerDeps.contains(where: { $0.contains("winmm") || $0.contains("xaudio") || $0.contains("dsound") || $0.contains("x3daudio") })
        result.hasInput = lowerDeps.contains(where: { $0.contains("xinput") || $0.contains("dinput") || $0.contains("rawinput") })
        result.hasNetwork = lowerDeps.contains(where: { $0.contains("ws2_32") || $0.contains("wininet") || $0.contains("winhttp") })
        result.hasCOM = lowerDeps.contains(where: { $0.contains("ole32") || $0.contains("shell32") || $0.contains("comctl32") })
        result.hasInstaller = lowerDeps.contains(where: { $0.contains("msi") || $0.contains("setupapi") })

        // D3D version
        if lowerDeps.contains(where: { $0.contains("d3d12") }) {
            result.d3dVersion = "d3d12"
            result.graphicsAPI = "D3D12"
        } else if lowerDeps.contains(where: { $0.contains("d3d11") }) {
            result.d3dVersion = "d3d11"
            result.graphicsAPI = "D3D11"
        } else if lowerDeps.contains(where: { $0.contains("d3d10") }) {
            result.d3dVersion = "d3d10"
            result.graphicsAPI = "D3D10"
        } else if lowerDeps.contains(where: { $0.contains("d3d9") }) {
            result.d3dVersion = "d3d9"
            result.graphicsAPI = "D3D9"
        } else if lowerDeps.contains(where: { $0.contains("opengl") }) {
            result.graphicsAPI = "OpenGL"
        } else if lowerDeps.contains(where: { $0.contains("vulkan") }) {
            result.graphicsAPI = "Vulkan"
        }

        // Audio API
        if lowerDeps.contains(where: { $0.contains("xaudio2") }) {
            result.audioAPI = "XAudio2"
        } else if lowerDeps.contains(where: { $0.contains("dsound") }) {
            result.audioAPI = "DirectSound"
        } else if lowerDeps.contains(where: { $0.contains("winmm") }) {
            result.audioAPI = "WinMM"
        }

        // Input API
        if lowerDeps.contains(where: { $0.contains("xinput") }) {
            result.inputAPI = "XInput"
        } else if lowerDeps.contains(where: { $0.contains("dinput") }) {
            result.inputAPI = "DirectInput"
        }

        // Category
        if result.hasInstaller == true {
            result.category = .installer
        } else if result.hasGraphics == true && (result.subsystem == "gui" || result.subsystem == "windows") {
            if result.d3dVersion == "d3d12" {
                result.category = .d3d12
            } else if result.d3dVersion == "d3d11" || result.d3dVersion == "d3d10" || result.d3dVersion == "d3d9" {
                result.category = .d3d11
            } else {
                result.category = .gdi
            }
        } else if result.subsystem == "console" || result.subsystem == "console" {
            result.category = .console
        } else if result.subsystem == "gui" || result.subsystem == "windows" {
            result.category = .gui
        } else {
            result.category = .unknown
        }
    }

    private static func suggestLane(result: inout PEInspectionResult) {
        let arch = result.arch.lowercased()
        if arch.contains("arm64") || arch.contains("aarch64") {
            result.suggestedLane = "arm64"
        } else if arch.contains("amd64") || arch.contains("x86_64") || arch.contains("x64") {
            result.suggestedLane = "x64"
        } else if arch.contains("i386") || arch.contains("x86") {
            result.suggestedLane = "x86"
        } else {
            result.suggestedLane = "auto"
        }
    }
}
