import Foundation
import SwiftUI

struct PPMParser {
    static func parse(data: Data) -> NSImage? {
        let prefix = data.prefix(64)
        guard prefix.count >= 2, prefix[0] == 0x50, prefix[1] == 0x36 else { return nil }

        func nextNewline(from: Int) -> Int? {
            for i in from..<data.count {
                if data[i] == 0x0A { return i }
            }
            return nil
        }

        var pos = 2
        guard let nl1 = nextNewline(from: pos) else { return nil }
        pos = nl1 + 1
        guard let nl2 = nextNewline(from: pos) else { return nil }
        pos = nl2 + 1
        guard let nl3 = nextNewline(from: pos) else { return nil }
        pos = nl3 + 1

        let dimLine = String(data: data[(nl1+1)..<nl2], encoding: .ascii)
        let maxValLine = String(data: data[(nl2+1)..<nl3], encoding: .ascii)

        let dim = dimLine?.split(separator: " ").compactMap { Int($0) }
        guard let dim = dim, dim.count == 2 else { return nil }
        let width = dim[0]
        let height = dim[1]

        guard let maxVal = maxValLine?.trimmingCharacters(in: .whitespaces),
              let max = Int(maxVal), max == 255 else { return nil }

        let pixelData = data.subdata(in: pos..<data.count)
        guard pixelData.count == width * height * 3 else { return nil }

        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue)
        guard let provider = CGDataProvider(data: pixelData as CFData),
              let cgImage = CGImage(
                width: width,
                height: height,
                bitsPerComponent: 8,
                bitsPerPixel: 24,
                bytesPerRow: width * 3,
                space: colorSpace,
                bitmapInfo: bitmapInfo,
                provider: provider,
                decode: nil,
                shouldInterpolate: false,
                intent: .defaultIntent
              ) else { return nil }

        return NSImage(cgImage: cgImage, size: NSSize(width: width, height: height))
    }
}
