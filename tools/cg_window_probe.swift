#!/usr/bin/env swift
import CoreGraphics
import Foundation

func number(_ value: Any?) -> Double {
    if let value = value as? NSNumber {
        return value.doubleValue
    }
    if let value = value as? Double {
        return value
    }
    if let value = value as? Int {
        return Double(value)
    }
    return 0
}

func string(_ value: Any?) -> String {
    return value as? String ?? ""
}

func emit(_ payload: [String: Any]) {
    let data = try! JSONSerialization.data(withJSONObject: payload, options: [.sortedKeys])
    FileHandle.standardOutput.write(data)
    FileHandle.standardOutput.write(Data([0x0a]))
}

let query = CommandLine.arguments.dropFirst().joined(separator: " ").lowercased()
let infoList = CGWindowListCopyWindowInfo(
    [.optionOnScreenOnly, .excludeDesktopElements],
    kCGNullWindowID
) as? [[String: Any]] ?? []

var windows: [[String: Any]] = []
for info in infoList {
    let owner = string(info[kCGWindowOwnerName as String])
    let name = string(info[kCGWindowName as String])
    let windowNumber = number(info[kCGWindowNumber as String])
    let bounds = info[kCGWindowBounds as String] as? [String: Any] ?? [:]

    let width = Int(number(bounds["Width"]))
    let height = Int(number(bounds["Height"]))
    if Int(windowNumber) == 0 || width <= 0 || height <= 0 {
        continue
    }

    windows.append([
        "window_id": Int(windowNumber),
        "owner": owner,
        "name": name,
        "pid": Int(number(info[kCGWindowOwnerPID as String])),
        "layer": Int(number(info[kCGWindowLayer as String])),
        "alpha": number(info[kCGWindowAlpha as String]),
        "x": Int(number(bounds["X"])),
        "y": Int(number(bounds["Y"])),
        "width": width,
        "height": height
    ])
}

emit([
    "status": "PASS",
    "query": query,
    "count": windows.count,
    "windows": windows
])
