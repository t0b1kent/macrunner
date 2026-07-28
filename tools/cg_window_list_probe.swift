// MacRunner 2026-07-28 (HK input lane): list the WindowServer's on-screen windows
// for a pid, from outside the process.
//
// Companion to ls_activation_probe.swift.  A Cocoa window only reaches the
// WindowServer's on-screen list once it has been ordered in, so this
// distinguishes "-[WineWindow orderBelow:orAbove:activate:] never ran" (which
// would explain a missing transformProcessToForeground) from "it ran and
// -[NSApplication setActivationPolicy:] failed".
//
// Build: swiftc -O tools/cg_window_list_probe.swift -o /tmp/mr-agents/cg_list
// Run:   /tmp/mr-agents/cg_list <pid> [<pid> ...]

import CoreGraphics
import Foundation

let wanted = Set(CommandLine.arguments.dropFirst().compactMap { Int32($0) })
if wanted.isEmpty {
    FileHandle.standardError.write("usage: cg_list <pid> [<pid> ...]\n".data(using: .utf8)!)
    exit(2)
}

func dump(_ label: String, _ options: CGWindowListOption) {
    guard let list = CGWindowListCopyWindowInfo(options, kCGNullWindowID) as? [[String: Any]] else {
        print("\(label): <query failed>")
        return
    }
    var n = 0
    for w in list {
        guard let pid = w[kCGWindowOwnerPID as String] as? Int32, wanted.contains(pid) else { continue }
        n += 1
        let num   = w[kCGWindowNumber as String] as? Int ?? -1
        let name  = w[kCGWindowName as String] as? String ?? ""
        let owner = w[kCGWindowOwnerName as String] as? String ?? ""
        let layer = w[kCGWindowLayer as String] as? Int ?? -999
        let alpha = w[kCGWindowAlpha as String] as? Double ?? -1
        let onscr = w[kCGWindowIsOnscreen as String] as? Bool ?? false
        let b     = w[kCGWindowBounds as String] as? [String: Any] ?? [:]
        print("\(label): pid=\(pid) win=\(num) layer=\(layer) onscreen=\(onscr) alpha=\(alpha) "
            + "owner=\"\(owner)\" title=\"\(name)\" bounds=\(b["X"] ?? "?"),\(b["Y"] ?? "?") "
            + "\(b["Width"] ?? "?")x\(b["Height"] ?? "?")")
    }
    if n == 0 { print("\(label): no windows for the requested pids") }
}

dump("onscreen-only", [.optionOnScreenOnly, .excludeDesktopElements])
dump("all-windows", [.optionAll])
