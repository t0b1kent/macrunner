import SwiftUI

struct TriageDetailView: View {
    let diagnosis: FailureClassifier.Diagnosis

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Image(systemName: iconFor(diagnosis.classification))
                    .font(.largeTitle)
                    .foregroundStyle(colorFor(diagnosis.classification))
                VStack(alignment: .leading) {
                    Text(diagnosis.classification.rawValue)
                        .font(.title2)
                    ConfidenceBar(confidence: diagnosis.confidence)
                }
                Spacer()
            }

            GroupBox("Cause") {
                Text(diagnosis.cause)
                    .textSelection(.enabled)
            }

            GroupBox("Evidence") {
                Text(diagnosis.evidence)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .textSelection(.enabled)
            }

            GroupBox("Recommended Action") {
                Text(diagnosis.recommendedCommand)
                    .textSelection(.enabled)
            }

            if !diagnosis.actions.isEmpty {
                GroupBox("Quick Actions") {
                    FlowActions(actions: diagnosis.actions)
                }
            }

            if !diagnosis.bundleFiles.isEmpty {
                GroupBox("Relevant Files") {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(diagnosis.bundleFiles, id: \.self) { file in
                            Label(file, systemImage: "doc")
                                .font(.caption)
                                .foregroundStyle(.secondary)
                        }
                    }
                }
            }

            Spacer()
        }
        .padding()
        .frame(minWidth: 500, minHeight: 400)
    }

    private func colorFor(_ classification: FailureClassifier.FailureClass) -> Color {
        switch classification {
        case .timeout, .cleanupFailed: return .orange
        case .crash, .invalidExe, .missingDll, .d3dValidationError, .metalUnavailable: return .red
        case .d3dShimNotLoaded, .archRoutingError, .prefixBroken: return .purple
        case .unknown: return .secondary
        }
    }

    private func iconFor(_ classification: FailureClassifier.FailureClass) -> String {
        switch classification {
        case .timeout: return "clock.badge.exclamationmark.fill"
        case .crash: return "xmark.octagon.fill"
        case .missingDll: return "square.and.arrow.down.fill"
        case .invalidExe: return "doc.badge.xmark"
        case .d3dValidationError, .d3dShimNotLoaded: return "cube.transparent"
        case .metalUnavailable: return "cpu"
        case .cleanupFailed: return "trash.slash.fill"
        case .prefixBroken: return "archivebox.slash.fill"
        case .archRoutingError: return "arrow.triangle.branch"
        case .unknown: return "questionmark.circle.fill"
        }
    }
}

private struct ConfidenceBar: View {
    let confidence: Double

    var body: some View {
        HStack(spacing: 4) {
            GeometryReader { geo in
                ZStack(alignment: .leading) {
                    Rectangle()
                        .fill(Color.secondary.opacity(0.2))
                        .cornerRadius(4)
                    Rectangle()
                        .fill(barColor)
                        .frame(width: geo.size.width * CGFloat(confidence))
                        .cornerRadius(4)
                }
            }
            .frame(height: 8)
            Text("\(Int(confidence * 100))%")
                .font(.caption2)
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
    }

    private var barColor: Color {
        if confidence >= 0.9 { return .green }
        if confidence >= 0.7 { return .orange }
        return .red
    }
}

private struct FlowActions: View {
    let actions: [String]

    var body: some View {
        LazyVGrid(columns: [GridItem(.adaptive(minimum: 120))], alignment: .leading, spacing: 8) {
            ForEach(actions, id: \.self) { action in
                Text(action)
                    .font(.caption)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(Color.accentColor.opacity(0.15))
                    .cornerRadius(4)
            }
        }
    }
}
