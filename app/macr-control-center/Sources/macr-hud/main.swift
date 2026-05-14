import AppKit
import Foundation
import MetalKit

final class HUDOverlayWindow: NSWindow {
    override var canBecomeKey: Bool { false }
    override var canBecomeMain: Bool { false }
}

final class HUDAppDelegate: NSObject, NSApplicationDelegate {
    private var window: HUDOverlayWindow?
    private var monitor: Any?

    func applicationDidFinishLaunching(_ notification: Notification) {
        let args = CommandLine.arguments
        if args.contains("--check") {
            print("macr-hud: ok")
            NSApp.terminate(nil)
            return
        }
        createWindow()
        monitor = NSEvent.addGlobalMonitorForEvents(matching: .keyDown) { [weak self] event in
            if event.modifierFlags.contains([.command, .shift]), event.charactersIgnoringModifiers?.lowercased() == "m" {
                self?.toggle()
            }
        }
    }

    private func createWindow() {
        let frame = NSRect(x: 32, y: NSScreen.main?.frame.height ?? 800 - 160, width: 280, height: 132)
        let window = HUDOverlayWindow(contentRect: frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.level = .screenSaver
        window.ignoresMouseEvents = true
        window.isOpaque = false
        window.backgroundColor = .clear
        window.contentView = HUDView(frame: NSRect(x: 0, y: 0, width: 280, height: 132))
        window.orderFrontRegardless()
        self.window = window
    }

    private func toggle() {
        guard let window else { return }
        window.isVisible ? window.orderOut(nil) : window.orderFrontRegardless()
    }
}

final class HUDView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        NSColor.black.withAlphaComponent(0.62).setFill()
        NSBezierPath(roundedRect: bounds, xRadius: 14, yRadius: 14).fill()
        let text = "FPS 60\nFrame 16.6 ms\nCache 95%"
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedDigitSystemFont(ofSize: 18, weight: .semibold), .foregroundColor: NSColor.white]
        text.draw(in: bounds.insetBy(dx: 18, dy: 18), withAttributes: attrs)
    }
}

let app = NSApplication.shared
let delegate = HUDAppDelegate()
app.delegate = delegate
app.setActivationPolicy(.accessory)
app.run()
