import Foundation
import Testing
@testable import MacRunnerControlCenter

struct FailureClassifierTests {
    @Test func classifyTimeout() {
        let result = LauncherResult(schemaVersion: 2, exe: nil, exePath: nil, arch: "arm64", machine: nil, executionLane: nil, status: "TIMEOUT", rc: 124, exitCode: nil, stdout: nil, stdoutPath: nil, stderrPath: nil, stderrTail: nil, durationMs: 10000, timedOut: true, timeout: true, crashed: false, cleanupOk: true, leftoversCount: 0, d3dEnabled: false, d3dBackend: nil, d3dStatus: nil, d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil, d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: nil, d3dValidationErrors: nil, metalDeviceDetected: nil, args: nil, envOverrides: nil, workdir: nil, command: nil, error: nil, cleanup: nil)
        let d = FailureClassifier.classify(result: result)
        #expect(d.classification == FailureClassifier.FailureClass.timeout)
    }

    @Test func classifyCrash() {
        let result = LauncherResult(schemaVersion: 2, exe: nil, exePath: nil, arch: "arm64", machine: nil, executionLane: nil, status: "CRASH", rc: -11, exitCode: nil, stdout: nil, stdoutPath: nil, stderrPath: nil, stderrTail: "Unhandled exception: c0000005", durationMs: 500, timedOut: false, timeout: false, crashed: true, cleanupOk: true, leftoversCount: 0, d3dEnabled: false, d3dBackend: nil, d3dStatus: nil, d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil, d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: nil, d3dValidationErrors: nil, metalDeviceDetected: nil, args: nil, envOverrides: nil, workdir: nil, command: nil, error: nil, cleanup: nil)
        let d = FailureClassifier.classify(result: result)
        #expect(d.classification == FailureClassifier.FailureClass.crash)
    }

    @Test func classifyMissingDll() {
        let result = LauncherResult(schemaVersion: 2, exe: nil, exePath: nil, arch: "x64", machine: nil, executionLane: nil, status: "MISSING_DLL", rc: 1, exitCode: nil, stdout: nil, stdoutPath: nil, stderrPath: nil, stderrTail: "module not found: d3d11.dll", durationMs: 200, timedOut: false, timeout: false, crashed: false, cleanupOk: true, leftoversCount: 0, d3dEnabled: false, d3dBackend: nil, d3dStatus: nil, d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil, d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: nil, d3dValidationErrors: nil, metalDeviceDetected: nil, args: nil, envOverrides: nil, workdir: nil, command: nil, error: nil, cleanup: nil)
        let d = FailureClassifier.classify(result: result)
        #expect(d.classification == FailureClassifier.FailureClass.missingDll)
    }

    @Test func classifyInvalidExe() {
        let result = LauncherResult(schemaVersion: 2, exe: nil, exePath: nil, arch: nil, machine: nil, executionLane: nil, status: "INVALID_EXE", rc: 2, exitCode: nil, stdout: nil, stdoutPath: nil, stderrPath: nil, stderrTail: nil, durationMs: 0, timedOut: false, timeout: false, crashed: false, cleanupOk: true, leftoversCount: 0, d3dEnabled: false, d3dBackend: nil, d3dStatus: nil, d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil, d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: nil, d3dValidationErrors: nil, metalDeviceDetected: nil, args: nil, envOverrides: nil, workdir: nil, command: nil, error: "file missing", cleanup: nil)
        let d = FailureClassifier.classify(result: result)
        #expect(d.classification == FailureClassifier.FailureClass.invalidExe)
    }

    @Test func classifyD3DValidation() {
        let result = LauncherResult(schemaVersion: 2, exe: nil, exePath: nil, arch: "x64", machine: nil, executionLane: nil, status: "FAIL", rc: 1, exitCode: nil, stdout: nil, stdoutPath: nil, stderrPath: nil, stderrTail: nil, durationMs: 2000, timedOut: false, timeout: false, crashed: false, cleanupOk: true, leftoversCount: 0, d3dEnabled: true, d3dBackend: "mock", d3dStatus: "FAIL", d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil, d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: 3, d3dValidationErrors: ["bad"], metalDeviceDetected: false, args: nil, envOverrides: nil, workdir: nil, command: nil, error: nil, cleanup: nil)
        let d = FailureClassifier.classify(result: result)
        #expect(d.classification == FailureClassifier.FailureClass.d3dValidationError)
    }

    @Test func classifyCleanupFailed() {
        let result = LauncherResult(schemaVersion: 2, exe: nil, exePath: nil, arch: "arm64", machine: nil, executionLane: nil, status: "FAIL", rc: 1, exitCode: nil, stdout: nil, stdoutPath: nil, stderrPath: nil, stderrTail: nil, durationMs: 1000, timedOut: false, timeout: false, crashed: false, cleanupOk: false, leftoversCount: 2, d3dEnabled: false, d3dBackend: nil, d3dStatus: nil, d3dTracePath: nil, d3dIrPath: nil, d3dReportPath: nil, d3dPpmPath: nil, d3dOutputChecksum: nil, d3dNonBackgroundPixels: nil, d3dUnsupportedCalls: nil, d3dValidationErrors: nil, metalDeviceDetected: nil, args: nil, envOverrides: nil, workdir: nil, command: nil, error: nil, cleanup: nil)
        let d = FailureClassifier.classify(result: result)
        #expect(d.classification == FailureClassifier.FailureClass.cleanupFailed)
    }

    @Test func classifyNilResult() {
        let d = FailureClassifier.classify(result: nil)
        #expect(d.classification == FailureClassifier.FailureClass.unknown)
    }
}
