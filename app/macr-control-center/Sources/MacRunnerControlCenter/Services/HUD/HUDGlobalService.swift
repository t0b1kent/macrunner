import AppKit
import Foundation

struct HUDFrameSample: Codable, Equatable {
    var fps: Double
    var frameMs: Double
    var gpuMs: Double?
    var cpuMs: Double?
    var cacheHits: Int
    var cacheMisses: Int

    enum CodingKeys: String, CodingKey {
        case fps
        case frameMs = "frame_ms"
        case gpuMs = "gpu_ms"
        case cpuMs = "cpu_ms"
        case cacheHits = "cache_hits"
        case cacheMisses = "cache_misses"
    }

    var cacheHitRate: Double {
        let total = cacheHits + cacheMisses
        return total == 0 ? 0 : Double(cacheHits) / Double(total)
    }
}

struct HUDRollingBuffer: Equatable {
    private(set) var samples: [HUDFrameSample] = []
    var capacity = 60

    mutating func append(_ sample: HUDFrameSample) {
        samples.append(sample)
        if samples.count > capacity { samples.removeFirst(samples.count - capacity) }
    }
}

struct HUDSocketConfig: Codable, Equatable {
    var socketPath: String
    var hotkey: String

    static func `default`(settings: AppSettings) -> HUDSocketConfig {
        let socket = URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("run/hud.sock").path
        return HUDSocketConfig(socketPath: socket, hotkey: settings.hudHotkey ?? "Cmd+Shift+M")
    }
}

struct HUDLauncher {
    func launchPlan(settings: AppSettings) -> ProcessInvocation {
        let hudBinary = Bundle.main.resourceURL?.appendingPathComponent("macr-hud").path ?? "macr-hud"
        let config = HUDSocketConfig.default(settings: settings)
        return ProcessInvocation(executable: hudBinary, arguments: ["--socket", config.socketPath, "--hotkey", config.hotkey], currentDirectory: settings.macRunnerRoot, environment: ["MACRUNNER_ROOT": settings.macRunnerRoot])
    }
}
