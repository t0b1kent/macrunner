import AppKit
import CryptoKit
import Foundation

struct SteamCoverFetchResult: Codable, Equatable {
    var appid: String
    var path: String
    var sourceURL: String?
    var bytes: Int64
    var placeholderGenerated: Bool
    var lastModified: String?
}

private struct SteamCoverMetadata: Codable, Equatable {
    var appid: String
    var sourceURL: String
    var lastModified: String?
    var fetchedAt: Date
}

actor SteamCoverFetchGate {
    static let shared = SteamCoverFetchGate(limit: 4)
    private let limit: Int
    private var available: Int
    private var waiters: [CheckedContinuation<Void, Never>] = []

    init(limit: Int) {
        self.limit = limit
        self.available = limit
    }

    func acquire() async {
        if available > 0 {
            available -= 1
            return
        }
        await withCheckedContinuation { continuation in
            waiters.append(continuation)
        }
    }

    func release() {
        if waiters.isEmpty {
            available = min(limit, available + 1)
        } else {
            waiters.removeFirst().resume()
        }
    }
}

struct SteamCoverFetcher {
    var root: URL = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first!
        .appendingPathComponent("MacRunner/covers/steam", isDirectory: true)
    var session: URLSession = .shared

    func fetch(appid: String, title: String? = nil) async throws -> SteamCoverFetchResult {
        await SteamCoverFetchGate.shared.acquire()
        defer { Task { await SteamCoverFetchGate.shared.release() } }

        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        let safeID = safe(appid)
        let coverURL = root.appendingPathComponent("\(safeID).jpg")
        let metadataURL = root.appendingPathComponent("\(safeID).jpg.json")
        let metadata = readMetadata(metadataURL)
        var sawNotFound = false

        for source in coverSources(appid: appid) {
            var request = URLRequest(url: source)
            request.setValue("MacRunner/1.0", forHTTPHeaderField: "User-Agent")
            if metadata?.sourceURL == source.absoluteString, let lastModified = metadata?.lastModified {
                request.setValue(lastModified, forHTTPHeaderField: "If-Modified-Since")
            }

            let (data, response) = try await fetchWithRetry(request)
            guard let http = response as? HTTPURLResponse else { throw SteamCoverError.invalidResponse }
            if http.statusCode == 304, FileManager.default.fileExists(atPath: coverURL.path) {
                return SteamCoverFetchResult(appid: appid, path: coverURL.path, sourceURL: source.absoluteString, bytes: fileSize(coverURL), placeholderGenerated: false, lastModified: metadata?.lastModified)
            }
            if http.statusCode == 404 {
                sawNotFound = true
                continue
            }
            guard (200..<300).contains(http.statusCode) else { throw SteamCoverError.badStatus(http.statusCode, source.absoluteString) }
            guard data.starts(with: [0xff, 0xd8]) else { throw SteamCoverError.notJPEG(source.absoluteString) }
            try atomicWrite(data, to: coverURL)
            let lastModified = http.value(forHTTPHeaderField: "Last-Modified")
            let nextMetadata = SteamCoverMetadata(appid: appid, sourceURL: source.absoluteString, lastModified: lastModified, fetchedAt: Date())
            try JSONEncoder().encode(nextMetadata).write(to: metadataURL)
            return SteamCoverFetchResult(appid: appid, path: coverURL.path, sourceURL: source.absoluteString, bytes: Int64(data.count), placeholderGenerated: false, lastModified: lastModified)
        }

        guard sawNotFound else { throw SteamCoverError.noSourceTried }
        let fallback = try generatedFallbackJPEG(title: title?.isEmpty == false ? title! : "Steam \(appid)")
        try atomicWrite(fallback, to: coverURL)
        try? FileManager.default.removeItem(at: metadataURL)
        return SteamCoverFetchResult(appid: appid, path: coverURL.path, sourceURL: nil, bytes: Int64(fallback.count), placeholderGenerated: true, lastModified: nil)
    }

    func cachedURL(appid: String) -> URL? {
        let url = root.appendingPathComponent("\(safe(appid)).jpg")
        return FileManager.default.fileExists(atPath: url.path) ? url : nil
    }

    func coverSources(appid: String) -> [URL] {
        [
            URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/\(appid)/library_600x900_2x.jpg")!,
            URL(string: "https://cdn.cloudflare.steamstatic.com/steam/apps/\(appid)/library_600x900.jpg")!,
            URL(string: "https://shared.akamai.steamstatic.com/store_item_assets/steam/apps/\(appid)/library_hero.jpg")!
        ]
    }

    private func readMetadata(_ url: URL) -> SteamCoverMetadata? {
        guard let data = try? Data(contentsOf: url) else { return nil }
        return try? JSONDecoder().decode(SteamCoverMetadata.self, from: data)
    }

    private func fetchWithRetry(_ request: URLRequest, attempts: Int = 3) async throws -> (Data, URLResponse) {
        var lastError: Error?
        for attempt in 1...attempts {
            do {
                return try await session.data(for: request)
            } catch {
                lastError = error
                if attempt < attempts {
                    try? await Task.sleep(nanoseconds: UInt64(attempt) * 500_000_000)
                }
            }
        }
        throw lastError ?? URLError(.networkConnectionLost)
    }

    private func generatedFallbackJPEG(title: String) throws -> Data {
        let image = NSImage(size: NSSize(width: 600, height: 900))
        image.lockFocus()
        NSColor(calibratedRed: 0.10, green: 0.12, blue: 0.16, alpha: 1).setFill()
        NSRect(x: 0, y: 0, width: 600, height: 900).fill()
        let digest = SHA256.hash(data: Data(title.utf8))
        let accent = CGFloat(Array(digest).first ?? 120) / 255.0
        NSColor(calibratedHue: accent, saturation: 0.72, brightness: 0.84, alpha: 1).setFill()
        NSBezierPath(roundedRect: NSRect(x: 56, y: 72, width: 488, height: 756), xRadius: 56, yRadius: 56).fill()
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.boldSystemFont(ofSize: 48),
            .foregroundColor: NSColor.white
        ]
        title.draw(in: NSRect(x: 84, y: 380, width: 432, height: 160), withAttributes: attrs)
        image.unlockFocus()
        guard let tiff = image.tiffRepresentation,
              let bitmap = NSBitmapImageRep(data: tiff),
              let jpg = bitmap.representation(using: .jpeg, properties: [.compressionFactor: 0.88])
        else { throw CocoaError(.fileWriteUnknown) }
        return jpg
    }

    private func atomicWrite(_ data: Data, to url: URL) throws {
        let tmp = url.deletingLastPathComponent().appendingPathComponent(".\(url.lastPathComponent).tmp")
        try data.write(to: tmp)
        if FileManager.default.fileExists(atPath: url.path) {
            _ = try FileManager.default.replaceItemAt(url, withItemAt: tmp)
        } else {
            try FileManager.default.moveItem(at: tmp, to: url)
        }
    }

    private func fileSize(_ url: URL) -> Int64 {
        let values = try? url.resourceValues(forKeys: [.fileSizeKey])
        return Int64(values?.fileSize ?? 0)
    }

    private func safe(_ text: String) -> String {
        text.map { $0.isLetter || $0.isNumber || $0 == "-" || $0 == "_" ? String($0) : "-" }.joined()
    }
}

enum SteamCoverError: LocalizedError, Equatable {
    case invalidResponse
    case badStatus(Int, String)
    case notJPEG(String)
    case noSourceTried

    var errorDescription: String? {
        switch self {
        case .invalidResponse: return "Steam cover response was not HTTP."
        case .badStatus(let status, let url): return "Steam cover fetch failed with HTTP \(status): \(url)"
        case .notJPEG(let url): return "Steam cover fetch returned non-JPEG data: \(url)"
        case .noSourceTried: return "No Steam cover source was attempted."
        }
    }
}
