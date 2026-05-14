import Foundation

@MainActor
struct CodexPromptService {
    static func generateFixPrompt(result: LauncherResult?, diagnosis: FailureClassifier.Diagnosis?) -> String {
        guard let result = result else {
            return "No run result available. Please run the app and provide the launcher JSON output."
        }

        var lines: [String] = []
        lines.append("# MacRunner Fix Request")
        lines.append("")
        lines.append("## App")
        lines.append("- exe: \(result.exe ?? result.exePath ?? "unknown")")
        lines.append("- arch: \(result.arch ?? result.machine ?? "unknown")")
        lines.append("- status: \(result.status ?? "?")")
        lines.append("- rc: \(result.rc ?? result.exitCode ?? -1)")
        lines.append("- duration: \(result.durationMs ?? 0)ms")
        if let backend = result.d3dBackend {
            lines.append("- d3dBackend: \(backend)")
        }
        lines.append("")

        if let d = diagnosis {
            lines.append("## Classification")
            lines.append("- class: \(d.classification.rawValue)")
            lines.append("- confidence: \(Int(d.confidence * 100))%")
            lines.append("")
            lines.append("## Cause")
            lines.append(d.cause)
            lines.append("")
            lines.append("## Evidence")
            lines.append(d.evidence)
            lines.append("")
        }

        lines.append("## Stderr Tail")
        lines.append("```")
        lines.append(result.stderrTail ?? "(none)")
        lines.append("```")
        lines.append("")

        if let errs = result.d3dValidationErrors, !errs.isEmpty {
            lines.append("## D3D Validation Errors")
            for err in errs {
                lines.append("- \(err)")
            }
            lines.append("")
        }

        if let uc = result.d3dUnsupportedCalls, uc > 0 {
            lines.append("- d3dUnsupportedCalls: \(uc)")
            lines.append("")
        }

        lines.append("## Recommended Action")
        lines.append(diagnosis?.recommendedCommand ?? "Inspect logs and re-run with --debug.")
        lines.append("")
        lines.append("## Request")
        lines.append("Please analyze the failure and suggest the most likely root cause and fix. If this is a D3D/Metal issue, focus on the backend configuration or unsupported call paths. If it's a Wine/prefix issue, suggest bottle or DLL fixes.")

        return lines.joined(separator: "\n")
    }

    static func generateCorpusPrompt(entries: [CompatibilityEntry]) -> String {
        let regressed = entries.filter { CompatibilityStore.shared.isRegression(appId: $0.id) }
        let failing = entries.filter { ($0.lastStatus ?? "") != "PASS" }

        var lines: [String] = []
        lines.append("# MacRunner Corpus Analysis Request")
        lines.append("")
        lines.append("## Summary")
        lines.append("- total apps: \(entries.count)")
        lines.append("- regressions: \(regressed.count)")
        lines.append("- currently failing: \(failing.count)")
        lines.append("")

        if !regressed.isEmpty {
            lines.append("## Regressed Apps")
            for e in regressed {
                lines.append("- \(e.name) (\(e.arch ?? "?")) - was PASS, now \(e.lastStatus ?? "?")")
            }
            lines.append("")
        }

        if !failing.isEmpty {
            lines.append("## Failing Apps")
            for e in failing.prefix(20) {
                lines.append("- \(e.name) [\(e.category?.rawValue ?? "?")] status=\(e.lastStatus ?? "?") d3d=\(e.bestD3DBackend ?? "none") failures=\(e.failuresCount)")
            }
            if failing.count > 20 {
                lines.append("- ... and \(failing.count - 20) more")
            }
            lines.append("")
        }

        lines.append("## Request")
        lines.append("Analyze the compatibility patterns and suggest:")
        lines.append("1. Common root causes across failing apps")
        lines.append("2. Recommended D3D backend strategy")
        lines.append("3. Which apps are most likely fixable with prefix/DLL changes")
        lines.append("4. Priority order for investigation")

        return lines.joined(separator: "\n")
    }
}
