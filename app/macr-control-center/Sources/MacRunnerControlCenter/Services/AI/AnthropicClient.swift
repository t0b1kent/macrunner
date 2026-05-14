import Foundation

struct AnthropicDiagnosis: Codable, Equatable {
    struct Fix: Codable, Equatable {
        var kind: String
        var payload: [String: String]
    }

    var rootCause: String
    var fix: Fix
    var confidence: Double

    enum CodingKeys: String, CodingKey {
        case rootCause = "root_cause"
        case fix
        case confidence
    }
}

struct AnthropicStreamEvent: Equatable {
    var text: String
    var isDone: Bool
}

actor AnthropicRateLimiter {
    private var lastRequest: Date?
    var minimumInterval: TimeInterval = 1.0

    func waitTurn() async {
        guard let lastRequest else {
            self.lastRequest = Date()
            return
        }
        let elapsed = Date().timeIntervalSince(lastRequest)
        if elapsed < minimumInterval {
            try? await Task.sleep(nanoseconds: UInt64((minimumInterval - elapsed) * 1_000_000_000))
        }
        self.lastRequest = Date()
    }
}

final class AnthropicClient {
    let endpoint: URL
    let model: String
    let keyStore: AnthropicKeyStore
    let session: URLSession
    let rateLimiter: AnthropicRateLimiter

    init(
        endpoint: URL = URL(string: "https://api.anthropic.com/v1/messages")!,
        model: String = "claude-opus-4-7",
        keyStore: AnthropicKeyStore = AnthropicKeyStore(),
        session: URLSession = .shared,
        rateLimiter: AnthropicRateLimiter = AnthropicRateLimiter()
    ) {
        self.endpoint = endpoint
        self.model = model
        self.keyStore = keyStore
        self.session = session
        self.rateLimiter = rateLimiter
    }

    func diagnose(prompt: String, progress: @escaping @Sendable (AnthropicStreamEvent) -> Void = { _ in }) async throws -> AnthropicDiagnosis {
        guard let apiKey = keyStore.load(), !apiKey.isEmpty else { throw AnthropicClientError.missingAPIKey }
        let system = AISystemPrompt.load()
        let body: [String: Any] = [
            "model": model,
            "max_tokens": 1200,
            "stream": true,
            "system": system,
            "messages": [["role": "user", "content": prompt]]
        ]
        let data = try JSONSerialization.data(withJSONObject: body)
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue(apiKey, forHTTPHeaderField: "x-api-key")
        request.setValue("2023-06-01", forHTTPHeaderField: "anthropic-version")
        request.setValue("application/json", forHTTPHeaderField: "content-type")
        request.httpBody = data

        var lastError: Error?
        for attempt in 0..<3 {
            try Task.checkCancellation()
            await rateLimiter.waitTurn()
            do {
                let raw = try await stream(request: request, progress: progress)
                return try Self.parseDiagnosis(from: raw)
            } catch {
                lastError = error
                let delay = UInt64(pow(2.0, Double(attempt)) * 350_000_000)
                try? await Task.sleep(nanoseconds: delay)
            }
        }
        throw lastError ?? AnthropicClientError.emptyResponse
    }

    private func stream(request: URLRequest, progress: @escaping @Sendable (AnthropicStreamEvent) -> Void) async throws -> String {
        let (bytes, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse, (200..<300).contains(http.statusCode) else {
            throw AnthropicClientError.badStatus((response as? HTTPURLResponse)?.statusCode ?? -1)
        }
        var accumulated = ""
        for try await line in bytes.lines {
            try Task.checkCancellation()
            guard line.hasPrefix("data:") else { continue }
            let payload = line.dropFirst(5).trimmingCharacters(in: .whitespacesAndNewlines)
            if payload == "[DONE]" { break }
            guard let data = payload.data(using: .utf8),
                  let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
            else { continue }
            if let delta = object["delta"] as? [String: Any], let text = delta["text"] as? String {
                accumulated += text
                progress(AnthropicStreamEvent(text: text, isDone: false))
            }
            if let content = object["content"] as? [[String: Any]] {
                for item in content where item["type"] as? String == "text" {
                    if let text = item["text"] as? String {
                        accumulated += text
                        progress(AnthropicStreamEvent(text: text, isDone: false))
                    }
                }
            }
        }
        progress(AnthropicStreamEvent(text: "", isDone: true))
        return accumulated
    }

    static func parseDiagnosis(from text: String) throws -> AnthropicDiagnosis {
        let trimmed = extractJSONObject(from: text)
        guard let data = trimmed.data(using: .utf8) else { throw AnthropicClientError.emptyResponse }
        return try JSONDecoder().decode(AnthropicDiagnosis.self, from: data)
    }

    private static func extractJSONObject(from text: String) -> String {
        if let start = text.firstIndex(of: "{"), let end = text.lastIndex(of: "}"), start <= end {
            return String(text[start...end])
        }
        return text
    }
}

enum AnthropicClientError: LocalizedError, Equatable {
    case missingAPIKey
    case badStatus(Int)
    case emptyResponse

    var errorDescription: String? {
        switch self {
        case .missingAPIKey: return "Anthropic API key is not configured."
        case .badStatus(let code): return "Anthropic API returned HTTP \(code)."
        case .emptyResponse: return "Anthropic response did not contain structured JSON."
        }
    }
}

enum AISystemPrompt {
    static func load() -> String {
        if let url = Bundle.module.url(forResource: "AISystemPrompt", withExtension: "txt"),
           let text = try? String(contentsOf: url, encoding: .utf8) {
            return text
        }
        return "MacRunner translates and launches Windows apps on Apple Silicon using profiles, bottles, Wine, and Process-based engine handoff. Return structured JSON only."
    }
}
