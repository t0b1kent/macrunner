import CoreGraphics
import Darwin
import Foundation
import ImageIO
import AppKit
import ScreenCaptureKit

func emit(_ payload: [String: Any]) {
    let data = try! JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data([0x0a]))
}

func fail(_ message: String, path: String, width: Int = 0, height: Int = 0) -> Never {
    emit([
        "status": "FAIL",
        "error": message,
        "path": path,
        "width": width,
        "height": height,
        "nonblack": 0,
        "colorful": 0
    ])
    exit(1)
}

func appendLE16(_ value: UInt16, to data: inout Data) {
    data.append(UInt8(value & 0xff))
    data.append(UInt8((value >> 8) & 0xff))
}

func appendLE32(_ value: UInt32, to data: inout Data) {
    data.append(UInt8(value & 0xff))
    data.append(UInt8((value >> 8) & 0xff))
    data.append(UInt8((value >> 16) & 0xff))
    data.append(UInt8((value >> 24) & 0xff))
}

func writeBMP(path: String, pixels: [UInt8], width: Int, height: Int) throws {
    let rowStride = ((width * 3 + 3) / 4) * 4
    let pixelBytes = rowStride * height
    let fileSize = 14 + 40 + pixelBytes
    var data = Data(capacity: fileSize)

    data.append(0x42)
    data.append(0x4d)
    appendLE32(UInt32(fileSize), to: &data)
    appendLE16(0, to: &data)
    appendLE16(0, to: &data)
    appendLE32(54, to: &data)

    appendLE32(40, to: &data)
    appendLE32(UInt32(width), to: &data)
    appendLE32(UInt32(height), to: &data)
    appendLE16(1, to: &data)
    appendLE16(24, to: &data)
    appendLE32(0, to: &data)
    appendLE32(UInt32(pixelBytes), to: &data)
    appendLE32(2835, to: &data)
    appendLE32(2835, to: &data)
    appendLE32(0, to: &data)
    appendLE32(0, to: &data)

    let padding = rowStride - width * 3
    for y in stride(from: height - 1, through: 0, by: -1) {
        for x in 0..<width {
            let i = (y * width + x) * 4
            data.append(pixels[i + 2])
            data.append(pixels[i + 1])
            data.append(pixels[i])
        }
        if padding > 0 {
            data.append(contentsOf: repeatElement(UInt8(0), count: padding))
        }
    }

    let url = URL(fileURLWithPath: path)
    try FileManager.default.createDirectory(
        at: url.deletingLastPathComponent(),
        withIntermediateDirectories: true
    )
    try data.write(to: url, options: [.atomic])
}

func runScreencapture(arguments: [String], pngPath: String) -> (CGImage?, String) {
    let process = Process()
    process.executableURL = URL(fileURLWithPath: "/usr/sbin/screencapture")
    process.arguments = arguments + [pngPath]
    let stderrPipe = Pipe()
    process.standardError = stderrPipe

    do {
        try process.run()
        process.waitUntilExit()
    } catch {
        return (nil, "launch failed: \(error)")
    }

    let stderrData = stderrPipe.fileHandleForReading.readDataToEndOfFile()
    let stderrText = String(data: stderrData, encoding: .utf8) ?? ""
    guard process.terminationStatus == 0 else {
        return (nil, "rc=\(process.terminationStatus): \(stderrText)")
    }
    guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: pngPath) as CFURL, nil),
          let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
        return (nil, "capture succeeded but PNG could not be decoded")
    }
    return (image, "")
}

func boundsForWindow(_ windowID: CGWindowID) -> CGRect? {
    guard let raw = CGWindowListCopyWindowInfo([.optionIncludingWindow], windowID),
          let windows = raw as? [[String: Any]],
          let window = windows.first,
          let bounds = window[kCGWindowBounds as String] as? [String: Any] else {
        return nil
    }
    return CGRect(dictionaryRepresentation: bounds as CFDictionary)
}

@main
struct WindowCapture {
    @MainActor
    static func main() async {
        _ = NSApplication.shared
        guard CommandLine.arguments.count == 3 else {
            fail("usage: cg_window_capture.swift WINDOW_ID OUTPUT_BMP", path: "")
        }

        let outputPath = CommandLine.arguments[2]
        guard let parsedWindowID = UInt32(CommandLine.arguments[1]) else {
            fail("invalid window id: \(CommandLine.arguments[1])", path: outputPath)
        }

        let pngPath = outputPath + ".png"
        var image: CGImage?
        var captureMode = "ScreenCaptureKit-window"
        var errors: [String] = []

        do {
            let content = try await SCShareableContent.excludingDesktopWindows(
                false, onScreenWindowsOnly: true)
            guard let window = content.windows.first(where: { $0.windowID == parsedWindowID }) else {
                throw NSError(domain: "cg_window_capture", code: 1,
                              userInfo: [NSLocalizedDescriptionKey: "window id is not shareable"])
            }
            let filter = SCContentFilter(desktopIndependentWindow: window)
            let configuration = SCStreamConfiguration()
            configuration.width = max(1, Int(window.frame.width.rounded()))
            configuration.height = max(1, Int(window.frame.height.rounded()))
            configuration.showsCursor = false
            image = try await SCScreenshotManager.captureImage(
                contentFilter: filter, configuration: configuration)
        } catch {
            errors.append("ScreenCaptureKit: \(error)")
        }

        if image == nil {
            captureMode = "screencapture-window-id"
            let result = runScreencapture(
                arguments: ["-x", "-l", String(parsedWindowID)], pngPath: pngPath)
            image = result.0
            if image == nil {
                errors.append("screencapture window-id: \(result.1)")
            }
        }

        if image == nil, let bounds = boundsForWindow(parsedWindowID) {
            captureMode = "screencapture-window-region"
            let x = Int(bounds.origin.x.rounded(.down))
            let y = Int(bounds.origin.y.rounded(.down))
            let width = max(1, Int(bounds.width.rounded(.up)))
            let height = max(1, Int(bounds.height.rounded(.up)))
            let region = "\(x),\(y),\(width),\(height)"
            let result = runScreencapture(
                arguments: ["-x", "-R", region], pngPath: pngPath)
            image = result.0
            if image == nil {
                errors.append("screencapture region \(region): \(result.1)")
            }
        }

        guard let image else {
            fail(errors.joined(separator: "; "), path: outputPath)
        }

        let width = image.width
        let height = image.height
        let bytesPerRow = width * 4
        var pixels = [UInt8](repeating: 0, count: bytesPerRow * height)
        let colorSpace = CGColorSpaceCreateDeviceRGB()
        let bitmapInfo = CGImageAlphaInfo.premultipliedLast.rawValue

        let drewImage = pixels.withUnsafeMutableBytes { rawBuffer -> Bool in
            guard let baseAddress = rawBuffer.baseAddress,
                  let context = CGContext(
                      data: baseAddress,
                      width: width,
                      height: height,
                      bitsPerComponent: 8,
                      bytesPerRow: bytesPerRow,
                      space: colorSpace,
                      bitmapInfo: bitmapInfo
                  ) else {
                return false
            }
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }

        if !drewImage {
            fail("failed to draw captured image into RGBA buffer",
                 path: outputPath, width: width, height: height)
        }

        var nonblack = 0
        var colorful = 0
        for y in 0..<height {
            for x in 0..<width {
                let i = (y * width + x) * 4
                let r = Int(pixels[i])
                let g = Int(pixels[i + 1])
                let b = Int(pixels[i + 2])
                let maxChannel = max(r, max(g, b))
                let minChannel = min(r, min(g, b))
                if maxChannel > 4 {
                    nonblack += 1
                }
                if maxChannel > 16 && maxChannel - minChannel > 24 {
                    colorful += 1
                }
            }
        }

        do {
            try writeBMP(path: outputPath, pixels: pixels, width: width, height: height)
            try? FileManager.default.removeItem(atPath: pngPath)
        } catch {
            fail("failed to write BMP: \(error)",
                 path: outputPath, width: width, height: height)
        }

        emit([
            "status": "PASS",
            "path": outputPath,
            "width": width,
            "height": height,
            "nonblack": nonblack,
            "colorful": colorful,
            "capture_mode": captureMode,
            "fallback_errors": errors
        ])
    }
}
