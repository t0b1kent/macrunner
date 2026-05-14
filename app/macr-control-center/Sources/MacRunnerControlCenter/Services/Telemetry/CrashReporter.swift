import Foundation

#if canImport(CrashReporter)
import CrashReporter
#endif

struct CrashReporterRuntime {
    func startIfEnabled(_ enabled: Bool) -> String {
        guard enabled else { return "CrashReporter disabled" }
        #if canImport(CrashReporter)
        let config = PLCrashReporterConfig(signalHandlerType: .mach, symbolicationStrategy: [])
        guard let reporter = PLCrashReporter(configuration: config) else { return "CrashReporter init failed" }
        do {
            try reporter.enableAndReturnError()
            return "CrashReporter enabled"
        } catch {
            return "CrashReporter failed: \(error.localizedDescription)"
        }
        #else
        return "CrashReporter framework unavailable"
        #endif
    }
}
