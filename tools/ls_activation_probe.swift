// MacRunner 2026-07-28 (HK input lane): report the LaunchServices / activation
// state of a live process from OUTSIDE it.
//
// The winemac.drv Cocoa app is measurably healthy — [NSApp run] is on the main
// thread stack inside _DPSNextEvent and com.apple.NSEventThread exists — yet no
// NSEvent is ever dispatched and applicationDidBecomeActive never fires.  An app
// that macOS does not consider an activatable application receives no key events,
// so the question is whether LaunchServices knows about this pid at all and what
// activation policy is actually in effect.
//
// Build: swiftc -O tools/ls_activation_probe.swift -o /tmp/mr-agents/ls_probe
// Run:   /tmp/mr-agents/ls_probe <pid> [<pid> ...]

import AppKit

func policyName(_ p: NSApplication.ActivationPolicy) -> String {
    switch p {
    case .regular:    return "regular"
    case .accessory:  return "accessory"
    case .prohibited: return "prohibited"
    @unknown default: return "unknown(\(p.rawValue))"
    }
}

let args = CommandLine.arguments.dropFirst()
if args.isEmpty {
    FileHandle.standardError.write("usage: ls_probe <pid> [<pid> ...]\n".data(using: .utf8)!)
    exit(2)
}

let frontmost = NSWorkspace.shared.frontmostApplication
print("frontmost: pid=\(frontmost?.processIdentifier ?? -1) name=\(frontmost?.localizedName ?? "(nil)") bundle=\(frontmost?.bundleIdentifier ?? "(nil)")")

let running = NSWorkspace.shared.runningApplications
print("runningApplications count=\(running.count)")

for a in args {
    guard let pid = Int32(a) else { continue }

    guard let app = NSRunningApplication(processIdentifier: pid) else {
        // The decisive negative: LaunchServices has no application record for
        // this pid, so it can never be activated and never receives key events.
        print("pid=\(pid) NSRunningApplication=NIL — not registered as an application with LaunchServices")
        continue
    }

    print("pid=\(pid) policy=\(policyName(app.activationPolicy)) active=\(app.isActive) "
        + "finishedLaunching=\(app.isFinishedLaunching) hidden=\(app.isHidden) "
        + "terminated=\(app.isTerminated) bundleID=\(app.bundleIdentifier ?? "(nil)") "
        + "name=\(app.localizedName ?? "(nil)") "
        + "exe=\(app.executableURL?.lastPathComponent ?? "(nil)")")
}
