import SwiftUI

struct AppPackagingView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: AppPackagingViewModel

    init() {
        self._vm = StateObject(wrappedValue: AppPackagingViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            HStack {
                Text("Package .app")
                    .font(.title)
                Spacer()
            }
            .padding(.horizontal)

            Form {
                Picker("App", selection: $vm.selectedApp) {
                    Text("Select an app").tag(nil as AppEntry?)
                    ForEach(vm.apps) { app in
                        Text(app.name).tag(app as AppEntry?)
                    }
                }

                TextField("Bundle Name", text: $vm.bundleName)
                    .disabled(vm.selectedApp == nil)

                HStack {
                    TextField("Output Path", text: $vm.outputPath)
                    Button("Browse...") { vm.pickOutputDirectory() }
                }
            }
            .formStyle(.grouped)
            .padding(.horizontal)

            HStack {
                Button("Package") {
                    vm.package()
                }
                .disabled(vm.selectedApp == nil || vm.isPackaging)
                .keyboardShortcut(.defaultAction)

                if vm.isPackaging {
                    ProgressView("Packaging...")
                        .padding(.leading, 8)
                }
                Spacer()
            }
            .padding(.horizontal)

            if let result = vm.lastResult {
                HStack {
                    Image(systemName: result.hasPrefix("Failed") ? "xmark.circle.fill" : "checkmark.circle.fill")
                        .foregroundStyle(result.hasPrefix("Failed") ? .red : .green)
                    Text(result)
                        .font(.callout)
                    Spacer()
                }
                .padding()
                .background(Color(.controlBackgroundColor))
                .cornerRadius(8)
                .padding(.horizontal)
            }

            Spacer()
        }
        .frame(minWidth: 500, minHeight: 300)
        .onAppear {
            vm.settings = settingsVM.settings
            vm.refresh()
        }
    }
}
