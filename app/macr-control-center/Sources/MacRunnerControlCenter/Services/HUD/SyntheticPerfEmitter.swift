import Foundation

struct SyntheticPerfEmitter {
    var settings: AppSettings = .default
    var sampleRateHz: Int = 60
    var durationSeconds: TimeInterval = 5

    func run() throws -> Int {
        let runDir = URL(fileURLWithPath: settings.macRunnerRoot).appendingPathComponent("run", isDirectory: true)
        try FileManager.default.createDirectory(at: runDir, withIntermediateDirectories: true)
        let logURL = runDir.appendingPathComponent("hud.sock.log")
        FileManager.default.createFile(atPath: logURL.path, contents: nil)
        let handle = try FileHandle(forWritingTo: logURL)
        defer { try? handle.close() }
        let total = Int(durationSeconds * Double(sampleRateHz))
        for index in 0..<total {
            let fps = 58.0 + sin(Double(index) / 8.0) * 4.0
            let frameMs = 1000.0 / max(fps, 1.0)
            let sample = HUDFrameSample(fps: fps, frameMs: frameMs, gpuMs: frameMs * 0.52, cpuMs: frameMs * 0.32, cacheHits: 900 + index, cacheMisses: 20)
            let data = try JSONEncoder().encode(sample)
            handle.write(data)
            handle.write(Data("\n".utf8))
            usleep(useconds_t(1_000_000 / max(sampleRateHz, 1)))
        }
        return total
    }
}
