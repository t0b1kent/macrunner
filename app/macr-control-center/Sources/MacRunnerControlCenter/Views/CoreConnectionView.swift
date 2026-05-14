import SwiftUI

struct CoreConnectionView: View {
    @ObservedObject var vm: CoreStatusViewModel
    var rootPath: String

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                CoreHealthBadge(score: vm.status.healthScore, description: vm.status.statusDescription)
                Spacer()
                Button("Refresh") { vm.refresh(root: rootPath) }
                    .disabled(vm.isChecking)
                if vm.isChecking {
                    ProgressView().scaleEffect(0.8)
                }
            }

            if !vm.status.isConnected {
                if !vm.status.isRootValid {
                    Label("Invalid MacRunner root: \(rootPath)", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.red)
                }

                if !vm.status.missingScripts.isEmpty {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Missing Scripts (\(vm.status.missingScripts.count)):")
                            .font(.caption.bold())
                            .foregroundStyle(.red)
                        ForEach(vm.status.missingScripts, id: \.self) { script in
                            Text(script)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                }

                if vm.status.missingReportsDir {
                    Label("Reports directory missing (auto-created)", systemImage: "folder.badge.plus")
                        .foregroundStyle(.orange)
                        .font(.caption)
                }

                if vm.status.missingArtifactsDir {
                    Label("Artifacts directory missing (auto-created)", systemImage: "folder.badge.plus")
                        .foregroundStyle(.orange)
                        .font(.caption)
                }
            } else {
                HStack {
                    Image(systemName: "checkmark.seal.fill")
                        .foregroundStyle(.green)
                    Text("Connected to MacRunner core")
                        .foregroundStyle(.green)
                }
            }

            if !vm.status.scriptHealth.isEmpty {
                DisclosureGroup("Script Health") {
                    LazyVGrid(columns: [GridItem(.adaptive(minimum: 200))], spacing: 4) {
                        ForEach(vm.status.scriptHealth.sorted(by: { $0.key < $1.key }), id: \.key) { script, ok in
                            HStack {
                                Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle.fill")
                                    .foregroundStyle(ok ? .green : .red)
                                    .font(.caption2)
                                Text(URL(fileURLWithPath: script).lastPathComponent)
                                    .font(.caption)
                                Spacer()
                            }
                        }
                    }
                }
            }
        }
        .padding()
        .background(Color(.controlBackgroundColor))
        .cornerRadius(8)
        .onAppear {
            vm.refresh(root: rootPath)
        }
        .onChange(of: rootPath) { _, new in
            vm.refresh(root: new)
        }
    }
}

struct CoreHealthBadge: View {
    let score: Int
    let description: String

    var body: some View {
        HStack(spacing: 6) {
            Circle()
                .fill(color)
                .frame(width: 10, height: 10)
            Text(description)
                .font(.callout.bold())
            Text("\(score)%")
                .font(.caption)
                .monospacedDigit()
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(color.opacity(0.15))
        .cornerRadius(6)
    }

    private var color: Color {
        if score >= 90 { return .green }
        if score >= 70 { return .orange }
        return .red
    }
}
