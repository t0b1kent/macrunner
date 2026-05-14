import Foundation

struct FailureClassifier {
    enum FailureClass: String, Sendable, CaseIterable {
        case invalidExe = "INVALID_EXE"
        case missingDll = "MISSING_DLL"
        case prefixBroken = "PREFIX_BROKEN"
        case crash = "CRASH"
        case timeout = "TIMEOUT"
        case d3dShimNotLoaded = "D3D_SHIM_NOT_LOADED"
        case d3dValidationError = "D3D_VALIDATION_ERROR"
        case metalUnavailable = "METAL_UNAVAILABLE"
        case archRoutingError = "ARCH_ROUTING_ERROR"
        case cleanupFailed = "CLEANUP_FAILED"
        case unknown = "UNKNOWN"
    }

    struct Diagnosis: Sendable {
        var classification: FailureClass
        var confidence: Double
        var cause: String
        var evidence: String
        var recommendedCommand: String
        var actions: [String]
        var bundleFiles: [String]
    }

    static func classify(result: LauncherResult?) -> Diagnosis {
        guard let result = result else {
            return Diagnosis(
                classification: .unknown,
                confidence: 0.0,
                cause: "No result available.",
                evidence: "Launcher did not produce JSON.",
                recommendedCommand: "Run with --debug and inspect stdout/stderr.",
                actions: ["Run Doctor", "Export Debug Bundle"],
                bundleFiles: ["last-run.json", "last-stdout.log", "last-stderr.log"]
            )
        }

        let status = result.status ?? "UNKNOWN"
        let stderr = result.stderrTail ?? ""
        let lowerStderr = stderr.lowercased()

        if status == "INVALID_EXE" {
            return Diagnosis(
                classification: .invalidExe,
                confidence: 1.0,
                cause: "Executable is missing, unreadable, or not a valid PE.",
                evidence: "Status=INVALID_EXE; error: \(result.error ?? "")",
                recommendedCommand: "Verify path and use pe_inspector.py.",
                actions: ["Run PE Inspector", "Re-import App"],
                bundleFiles: ["last-run.json"]
            )
        }

        if status == "TIMEOUT" || result.timedOut == true {
            return Diagnosis(
                classification: .timeout,
                confidence: 1.0,
                cause: "App did not finish within timeout.",
                evidence: "Status=TIMEOUT; duration=\(result.durationMs ?? 0)ms",
                recommendedCommand: "Increase --timeout or check for hangs.",
                actions: ["Re-run with longer timeout", "Run Process Monitor"],
                bundleFiles: ["last-run.json", "last-stderr.log"]
            )
        }

        if status == "CRASH" || result.crashed == true || lowerStderr.contains("unhandled exception") || lowerStderr.contains("c0000005") {
            return Diagnosis(
                classification: .crash,
                confidence: 0.95,
                cause: "Unhandled exception or memory access violation.",
                evidence: "Status=CRASH; stderr contains exception/c0000005.",
                recommendedCommand: "Run with --debug and check Wine backtrace.",
                actions: ["Run with Debug", "Export Debug Bundle"],
                bundleFiles: ["last-run.json", "last-stderr.log"]
            )
        }

        if lowerStderr.contains("import_dll") || lowerStderr.contains("module not found") || lowerStderr.contains("failed to load") || status == "MISSING_DLL" {
            return Diagnosis(
                classification: .missingDll,
                confidence: 0.9,
                cause: "Required DLL not found in prefix or system path.",
                evidence: "Status=MISSING_DLL or stderr mentions missing module.",
                recommendedCommand: "Install required redistributables into bottle or use WINEDLLOVERRIDES.",
                actions: ["Open Bottle Manager", "Run Doctor"],
                bundleFiles: ["last-run.json", "last-stderr.log"]
            )
        }

        if result.d3dEnabled == true, result.d3dStatus?.uppercased() != "PASS" {
            if result.d3dBackend == "metal", result.metalDeviceDetected == false {
                return Diagnosis(
                    classification: .metalUnavailable,
                    confidence: 0.95,
                    cause: "Metal backend selected but Metal device not detected.",
                    evidence: "d3d_backend=metal but metal_device_detected=false.",
                    recommendedCommand: "Switch to mock backend or verify Metal support.",
                    actions: ["Switch to Mock Backend", "Run Verify"],
                    bundleFiles: ["last-run.json", "doctor.json"]
                )
            }
            if (result.d3dUnsupportedCalls ?? 0) > 0 {
                return Diagnosis(
                    classification: .d3dValidationError,
                    confidence: 0.9,
                    cause: "D3D trace replay encountered unsupported calls or validation errors.",
                    evidence: "d3d_unsupported_calls=\(result.d3dUnsupportedCalls ?? 0); validation_errors=\(result.d3dValidationErrors?.count ?? 0)",
                    recommendedCommand: "Review D3D trace and IR for unsupported calls.",
                    actions: ["Open D3D Workbench", "Export Debug Bundle"],
                    bundleFiles: ["last-run.json", "d3d_trace.jsonl", "d3d_report.json"]
                )
            }
            return Diagnosis(
                classification: .d3dShimNotLoaded,
                confidence: 0.8,
                cause: "D3D shim did not load or replay failed.",
                evidence: "d3d_enabled=true but d3d_status=\(result.d3dStatus ?? "?").",
                recommendedCommand: "Run D3D smoke test and check bridge build.",
                actions: ["Run D3D Smoke Test", "Run Verify"],
                bundleFiles: ["last-run.json", "d3d_report.json"]
            )
        }

        if result.cleanupOk == false || (result.leftoversCount ?? 0) > 0 {
            return Diagnosis(
                classification: .cleanupFailed,
                confidence: 0.85,
                cause: "Wine processes or temp directories remained after run.",
                evidence: "cleanup_ok=false; leftovers_count=\(result.leftoversCount ?? 0).",
                recommendedCommand: "Run cleanup-wine-runtime.py and assert-no-wine-leftovers.sh.",
                actions: ["Cleanup Runtime", "Run Process Monitor"],
                bundleFiles: ["last-run.json", "doctor.json"]
            )
        }

        if status == "FAIL" {
            return Diagnosis(
                classification: .unknown,
                confidence: 0.5,
                cause: "Run failed with non-zero exit code.",
                evidence: "Status=FAIL; rc=\(result.rc ?? result.exitCode ?? -1); stderr: \(stderr.prefix(200))",
                recommendedCommand: "Inspect stderr and adjust environment or arguments.",
                actions: ["Inspect Logs", "Run Doctor", "Export Debug Bundle"],
                bundleFiles: ["last-run.json", "last-stderr.log"]
            )
        }

        return Diagnosis(
            classification: .unknown,
            confidence: 0.3,
            cause: "Could not classify failure.",
            evidence: "Status=\(status).",
            recommendedCommand: "Run doctor and export debug bundle.",
            actions: ["Run Doctor", "Export Debug Bundle"],
            bundleFiles: ["last-run.json", "doctor.json", "last-stderr.log"]
        )
    }

    @MainActor
    static func classifyWithHistory(result: LauncherResult?, exePath: String? = nil) -> Diagnosis {
        var diagnosis = classify(result: result)
        guard let exePath = exePath else { return diagnosis }

        let entries = CompatibilityStore.shared.loadEntries()
        if let entry = entries.first(where: { $0.exePathHash == exePath }), entry.failuresCount > 2 {
            diagnosis.confidence = min(diagnosis.confidence + 0.15, 1.0)
            diagnosis.evidence += "; Compatibility DB shows \(entry.failuresCount) prior failures."
            if !diagnosis.actions.contains("Check Compatibility DB") {
                diagnosis.actions.append("Check Compatibility DB")
            }
        }

        return diagnosis
    }
}
