// All-black BORDERLESS AppKit window for the pixel-truth-gate BLACK negative
// self-test. Models the HK rung-11 case: a visible window exists but no frame
// was drawn. Borderless so no title-bar traffic-light pixels leak in.
import AppKit

final class BlackView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.setFill()
        bounds.fill()
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    var window: NSWindow?
    func applicationDidFinishLaunching(_ notification: Notification) {
        let rect = NSRect(x: 160, y: 160, width: 640, height: 420)
        let window = NSWindow(
            contentRect: rect,
            styleMask: .borderless,
            backing: .buffered, defer: false)
        window.title = "PixelGate Black"
        window.contentView = BlackView(frame: rect)
        window.isOpaque = true
        window.backgroundColor = NSColor.black
        window.isReleasedWhenClosed = false
        window.makeKeyAndOrderFront(nil)
        self.window = window
        NSApp.activate(ignoringOtherApps: true)
    }
}

let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate
app.setActivationPolicy(.regular)
app.run()
