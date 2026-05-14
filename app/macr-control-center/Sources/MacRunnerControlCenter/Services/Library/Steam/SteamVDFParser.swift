import Foundation

enum VDFNode: Equatable {
    case string(String)
    case number(Double)
    case object([String: VDFNode])

    subscript(key: String) -> VDFNode? {
        if case .object(let dict) = self { return dict[key] }
        return nil
    }

    var stringValue: String? {
        if case .string(let value) = self { return value }
        if case .number(let value) = self {
            return value.rounded() == value ? String(Int64(value)) : String(value)
        }
        return nil
    }

    var objectValue: [String: VDFNode]? {
        if case .object(let dict) = self { return dict }
        return nil
    }
}

struct SteamVDFParser {
    func parse(_ text: String) throws -> VDFNode {
        var tokenizer = VDFTokenizer(text: text)
        var dict: [String: VDFNode] = [:]
        while let key = tokenizer.nextToken() {
            dict[key] = try parseValue(tokenizer: &tokenizer)
        }
        return .object(dict)
    }

    private func parseValue(tokenizer: inout VDFTokenizer) throws -> VDFNode {
        if tokenizer.consumeBrace("{") {
            var dict: [String: VDFNode] = [:]
            while !tokenizer.consumeBrace("}") {
                guard let key = tokenizer.nextToken() else { throw VDFError.unexpectedEOF }
                dict[key] = try parseValue(tokenizer: &tokenizer)
            }
            return .object(dict)
        }
        guard let value = tokenizer.nextToken() else { throw VDFError.unexpectedEOF }
        if let number = Double(value), value.rangeOfCharacter(from: .letters) == nil {
            return .number(number)
        }
        return .string(value)
    }
}

struct VDFTokenizer {
    private var scalars: [Character]
    private var index = 0

    init(text: String) { self.scalars = Array(text) }

    mutating func nextToken() -> String? {
        skipWhitespaceAndComments()
        guard index < scalars.count else { return nil }
        if scalars[index] != "\"" {
            var output = ""
            while index < scalars.count, !scalars[index].isWhitespace, scalars[index] != "{", scalars[index] != "}" {
                output.append(scalars[index])
                index += 1
            }
            return output.isEmpty ? nil : output
        }
        index += 1
        var output = ""
        while index < scalars.count {
            let ch = scalars[index]
            index += 1
            if ch == "\"" { break }
            if ch == "\\", index < scalars.count {
                let escaped = scalars[index]
                index += 1
                switch escaped {
                case "n": output.append("\n")
                case "t": output.append("\t")
                case "\"": output.append("\"")
                case "\\": output.append("\\")
                default: output.append(escaped)
                }
                continue
            }
            output.append(ch)
        }
        return output
    }

    mutating func consumeBrace(_ brace: Character) -> Bool {
        skipWhitespaceAndComments()
        guard index < scalars.count, scalars[index] == brace else { return false }
        index += 1
        return true
    }

    private mutating func skipWhitespaceAndComments() {
        while index < scalars.count {
            if scalars[index].isWhitespace { index += 1; continue }
            if scalars[index] == "/", index + 1 < scalars.count, scalars[index + 1] == "/" {
                while index < scalars.count, scalars[index] != "\n" { index += 1 }
                continue
            }
            break
        }
    }
}

enum VDFError: LocalizedError { case unexpectedEOF }
