import SwiftUI

struct BottleManagerView: View {
    @EnvironmentObject var settingsVM: SettingsViewModel
    @StateObject private var vm: BottleManagerViewModel
    @State private var confirmDelete: BottleInfo?
    @State private var archiveName = ""
    @State private var showArchiveSheet = false
    @State private var selectedBottle: BottleInfo?
    @State private var showBatchDeleteConfirm = false
    @State private var selection: String?

    init() {
        self._vm = StateObject(wrappedValue: BottleManagerViewModel(settings: SettingsViewModel().settings))
    }

    var body: some View {
        VStack {
            HStack {
                Button("Refresh") { vm.refresh() }
                Button("Cleanup Dry-Run") { vm.dryRunCleanup() }
                Button("Delete All Stale") { showBatchDeleteConfirm = true }
                    .disabled(!vm.bottles.contains { $0.status == "stale" })
                Spacer()
            }
            .padding()

            if let results = vm.dryRunResults {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Cleanup Preview").font(.caption).foregroundStyle(.secondary)
                    ForEach(results, id: \.self) { r in
                        Text(r).font(.caption2)
                    }
                }
                .padding(.horizontal)
            }

            if vm.bottles.isEmpty {
                ContentUnavailableView("No bottles found", systemImage: "archivebox")
                    .frame(maxHeight: .infinity)
            } else {
                List(vm.bottles) { bottle in
                    HStack {
                        VStack(alignment: .leading, spacing: 4) {
                            Text(bottle.name).font(.headline)
                            Text(bottle.path).font(.caption).foregroundStyle(.secondary).lineLimit(1)
                            HStack {
                                Text(Formatters.byteCountString(bottle.sizeBytes))
                                    .font(.caption2).foregroundStyle(.secondary)
                                if let mod = bottle.modified {
                                    Text(Formatters.mediumDate.string(from: mod))
                                        .font(.caption2).foregroundStyle(.secondary)
                                }
                                if bottle.status == "stale" {
                                    Text("Stale").font(.caption2).foregroundStyle(.orange)
                                }
                                if let win = bottle.windowsVersion {
                                    Text(win).font(.caption2).foregroundStyle(.blue)
                                }
                            }
                        }
                        Spacer()
                        Button("Repair") {
                            vm.repair(bottle)
                        }
                        Button("Show in Finder") { vm.revealInFinder(bottle) }
                        Button("Archive") {
                            selectedBottle = bottle
                            archiveName = "\(bottle.name).zip"
                            showArchiveSheet = true
                        }
                        Button("Delete", role: .destructive) {
                            confirmDelete = bottle
                        }
                    }
                }
            }
        }
        .alert("Delete Bottle?", isPresented: .init(
            get: { confirmDelete != nil },
            set: { if !$0 { confirmDelete = nil } }
        )) {
            TextField("Type bottle name to confirm", text: $archiveName)
            Button("Cancel", role: .cancel) { confirmDelete = nil }
            Button("Delete", role: .destructive) {
                if let bottle = confirmDelete, archiveName == bottle.name {
                    _ = vm.delete(bottle, confirmed: true)
                }
                confirmDelete = nil
                archiveName = ""
            }
        } message: {
            if let bottle = confirmDelete {
                Text("Type '\(bottle.name)' to permanently delete.")
            }
        }
        .sheet(isPresented: $showArchiveSheet) {
            VStack {
                Text("Archive Bottle").font(.headline)
                TextField("Archive name", text: $archiveName)
                HStack {
                    Button("Cancel") { showArchiveSheet = false }
                    Button("Archive") {
                        if let bottle = selectedBottle {
                            vm.archive(bottle, to: archiveName)
                        }
                        showArchiveSheet = false
                    }
                }
            }
            .padding()
            .frame(width: 320, height: 160)
        }
        .sheet(isPresented: $vm.showRepairSheet) {
            VStack(alignment: .leading, spacing: 12) {
                HStack {
                    Text("Bottle Repair")
                        .font(.title2)
                    Spacer()
                    Button("Done") { vm.showRepairSheet = false }
                }
                if vm.isRepairing {
                    ProgressView("Repairing...")
                        .frame(maxWidth: .infinity, alignment: .center)
                } else {
                    ScrollView {
                        Text(vm.repairOutput)
                            .font(.system(.body, design: .monospaced))
                            .textSelection(.enabled)
                    }
                }
            }
            .padding()
            .frame(minWidth: 500, minHeight: 300)
        }
        .alert("Delete all stale bottles?", isPresented: $showBatchDeleteConfirm) {
            Button("Cancel", role: .cancel) {}
            Button("Delete All", role: .destructive) {
                vm.deleteAllStale()
            }
        } message: {
            let count = vm.bottles.filter { $0.status == "stale" }.count
            Text("This will permanently delete \(count) stale bottle(s).")
        }
        .onAppear {
            vm.settings = settingsVM.settings
            vm.refresh()
        }
    }
}
