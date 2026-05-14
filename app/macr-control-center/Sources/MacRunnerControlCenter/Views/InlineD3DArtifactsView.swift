import SwiftUI

struct InlineD3DArtifactsView: View {
    let result: LauncherResult
    @StateObject private var vm = D3DArtifactsViewModel()

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("D3D / Metal Artifacts").font(.headline)
                Spacer()
                if let backend = result.d3dBackend {
                    Text("Backend: \(backend)").font(.caption).foregroundStyle(.secondary)
                }
            }

            HStack(spacing: 16) {
                StatusBadge(status: result.d3dStatus)
                if result.metalDeviceDetected == true {
                    Label("Metal Detected", systemImage: "checkmark.circle.fill")
                        .foregroundStyle(.green)
                        .font(.caption)
                }
                if let uc = result.d3dUnsupportedCalls, uc > 0 {
                    Label("Unsupported: \(uc)", systemImage: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .font(.caption)
                }
                if let px = result.d3dNonBackgroundPixels {
                    Label("Pixels: \(px)", systemImage: "photo")
                        .foregroundStyle(.secondary)
                        .font(.caption)
                }
                if let cs = result.d3dOutputChecksum {
                    Text("Checksum: \(cs.prefix(16))…")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
                Spacer()
            }

            if let errors = result.d3dValidationErrors, !errors.isEmpty {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Validation Errors (\(errors.count))")
                        .font(.caption.bold())
                        .foregroundStyle(.red)
                    ForEach(errors.prefix(3), id: \.self) { err in
                        Text("• \(err)")
                            .font(.caption)
                            .foregroundStyle(.red)
                    }
                    if errors.count > 3 {
                        Text("… and \(errors.count - 3) more")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
            }

            if vm.ppmImage != nil {
                PPMImageView(image: vm.ppmImage)
                    .frame(maxHeight: 200)
                    .cornerRadius(6)
            }

            if !vm.traceLines.isEmpty {
                HStack {
                    Text("Trace: \(vm.traceLines.count) lines")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("View Trace") {
                        // Handled by parent navigation if needed
                    }
                    .font(.caption)
                    .disabled(true)
                }
            }

            if !vm.reportJSON.isEmpty {
                HStack {
                    Text("Report loaded")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("View Report") {}
                        .font(.caption)
                        .disabled(true)
                }
            }

            if !vm.irJSON.isEmpty {
                HStack {
                    Text("IR loaded")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    Spacer()
                    Button("View IR") {}
                        .font(.caption)
                        .disabled(true)
                }
            }
        }
        .padding()
        .background(Color(.controlBackgroundColor))
        .cornerRadius(8)
        .onAppear {
            vm.load(from: result)
        }
    }
}
