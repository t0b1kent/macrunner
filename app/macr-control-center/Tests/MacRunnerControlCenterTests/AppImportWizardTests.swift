import Foundation
import Testing
@testable import MacRunnerControlCenter

struct AppImportWizardTests {
    @Test func inspectionResultDefaults() {
        let r = PEInspectionResult(
            arch: "arm64",
            subsystem: "console",
            d3dDetected: false,
            suggestedD3DBackend: "none",
            suggestedTimeout: 45
        )
        #expect(r.arch == "arm64")
        #expect(r.d3dDetected == false)
    }

    @Test func inspectionResultD3D() {
        let r = PEInspectionResult(
            arch: "x64",
            subsystem: "gui",
            d3dDetected: true,
            suggestedD3DBackend: "mock",
            suggestedTimeout: 120
        )
        #expect(r.suggestedD3DBackend == "mock")
        #expect(r.suggestedTimeout == 120)
    }

    @Test func inspectionResultFullFields() {
        var r = PEInspectionResult(
            arch: "x64",
            subsystem: "gui",
            d3dDetected: true,
            suggestedD3DBackend: "metal",
            suggestedTimeout: 120,
            imports: ["d3d11.dll", "kernel32.dll"],
            exports: ["Init", "Run"],
            sections: [".text", ".data"],
            dllDependencies: ["d3d11.dll", "dxgi.dll"],
            isDotNet: false,
            is64Bit: true,
            fileSizeBytes: 1024,
            iconPath: "/tmp/icon.ico"
        )
        #expect(r.imports?.contains("d3d11.dll") == true)
        #expect(r.dllDependencies?.contains("dxgi.dll") == true)
        #expect(r.is64Bit == true)
        #expect(r.fileSizeBytes == 1024)
    }
}
