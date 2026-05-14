import Foundation

struct AIFixApplyPlan: Codable, Equatable {
    var summary: String
    var fileWrites: [String]
    var processInvocations: [ProcessInvocation]
}

struct AIFixApplier {
    func plan(diagnosis: AnthropicDiagnosis, bottlePath: String, profileID: String?, settings: AppSettings) -> AIFixApplyPlan {
        switch diagnosis.fix.kind {
        case "dll_override":
            let dll = diagnosis.fix.payload["dll"] ?? diagnosis.fix.payload["name"] ?? "unknown"
            let path = URL(fileURLWithPath: bottlePath).appendingPathComponent("macrunner-dll-overrides.json").path
            return AIFixApplyPlan(summary: "Write DLL override for \(dll)", fileWrites: [path], processInvocations: [])
        case "verb":
            let verb = diagnosis.fix.payload["verb"] ?? "unknown"
            let invocation = ProcessInvocation(executable: "/usr/bin/env", arguments: ["WINEPREFIX=\(bottlePath)", "winetricks", verb], currentDirectory: settings.macRunnerRoot, environment: ["WINEPREFIX": bottlePath])
            return AIFixApplyPlan(summary: "Run winetricks verb \(verb)", fileWrites: [], processInvocations: [invocation])
        case "profile_switch":
            let next = diagnosis.fix.payload["profile_id"] ?? profileID ?? "unknown"
            return AIFixApplyPlan(summary: "Switch profile to \(next)", fileWrites: ["profile metadata"], processInvocations: [])
        case "registry":
            let regPath = URL(fileURLWithPath: bottlePath).appendingPathComponent("macrunner-ai-fix.reg").path
            let invocation = ProcessInvocation(executable: "/usr/bin/env", arguments: ["WINEPREFIX=\(bottlePath)", "wine", "regedit", regPath], currentDirectory: settings.macRunnerRoot, environment: ["WINEPREFIX": bottlePath])
            return AIFixApplyPlan(summary: "Emit and apply registry fix", fileWrites: [regPath], processInvocations: [invocation])
        default:
            return AIFixApplyPlan(summary: "Unsupported fix kind \(diagnosis.fix.kind)", fileWrites: [], processInvocations: [])
        }
    }
}
