import SwiftUI

struct HomeView: View {
    @Environment(\.colorScheme) private var scheme
    @EnvironmentObject var settingsVM: SettingsViewModel
    @Binding var developerMode: Bool
    @StateObject private var vm = AppLibraryViewModel()

    @State private var showAdd = false
    @State private var showSettings = false
    @State private var editingApp: AppEntry?
    @State private var detailApp: AppEntry?
    @State private var runningIDs = Set<UUID>()
    @State private var searchFocused = false

    private let columns = [GridItem(.adaptive(minimum: 260, maximum: 320), spacing: Theme.Spacing.l, alignment: .top)]

    var body: some View {
        ZStack {
            Theme.Palette.bgPrimary(scheme).ignoresSafeArea()

            VStack(spacing: 0) {
                header
                    .padding(.horizontal, Theme.Spacing.xxl)
                    .padding(.top, Theme.Spacing.xxl)
                    .padding(.bottom, Theme.Spacing.l)

                Divider()
                    .background(Theme.Palette.separator(scheme))

                content
            }

            Button("") {
                developerMode = true
            }
            .keyboardShortcut("d", modifiers: [.command, .shift])
            .opacity(0)
            .frame(width: 0, height: 0)
            .accessibilityHidden(true)
        }
        .sheet(isPresented: $showAdd) {
            AddAppView(onSave: {
                vm.add($0)
                showAdd = false
            })
        }
        .sheet(item: $editingApp) { app in
            AddAppView(app: app, onSave: {
                vm.update($0)
                editingApp = nil
            })
        }
        .sheet(item: $detailApp) { app in
            AppDetailSheet(
                app: app,
                isRunning: runningIDs.contains(app.id),
                onRun: { runApp(app) },
                onEdit: {
                    detailApp = nil
                    editingApp = app
                },
                onDelete: {
                    vm.delete(app)
                    detailApp = nil
                },
                onClose: { detailApp = nil }
            )
            .environmentObject(settingsVM)
        }
        .sheet(isPresented: $showSettings) {
            MinimalSettingsSheet(
                developerMode: $developerMode,
                onClose: { showSettings = false }
            )
            .environmentObject(settingsVM)
        }
    }

    private var header: some View {
        HStack(alignment: .center, spacing: Theme.Spacing.l) {
            VStack(alignment: .leading, spacing: 2) {
                Text("MacRunner")
                    .font(Theme.Font.display)
                    .foregroundColor(Theme.Palette.textPrimary(scheme))
                    .kerning(-0.5)
                Text(libraryCountLabel)
                    .font(Theme.Font.caption)
                    .foregroundColor(Theme.Palette.textTertiary(scheme))
            }

            Spacer()

            searchField
                .frame(width: 280)

            HStack(spacing: Theme.Spacing.s) {
                Button {
                    showAdd = true
                } label: {
                    HStack(spacing: 6) {
                        Image(systemName: "plus")
                            .font(.system(size: 11, weight: .bold))
                        Text("Add")
                    }
                }
                .buttonStyle(.minimalPrimary)
                .keyboardShortcut("n", modifiers: .command)

                Button {
                    showSettings = true
                } label: {
                    Image(systemName: "gearshape")
                        .font(.system(size: 14, weight: .regular))
                        .foregroundColor(Theme.Palette.textSecondary(scheme))
                        .frame(width: 36, height: 36)
                        .background(
                            Circle()
                                .fill(Color.clear)
                                .overlay(
                                    Circle()
                                        .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                                )
                        )
                }
                .buttonStyle(.scalePress)
                .keyboardShortcut(",", modifiers: .command)
            }
        }
    }

    private var searchField: some View {
        HStack(spacing: Theme.Spacing.s) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 12, weight: .regular))
                .foregroundColor(Theme.Palette.textTertiary(scheme))
            TextField("Search", text: $vm.search)
                .textFieldStyle(.plain)
                .font(Theme.Font.body)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
            if !vm.search.isEmpty {
                Button {
                    vm.search = ""
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 12))
                        .foregroundColor(Theme.Palette.textTertiary(scheme))
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.vertical, 8)
        .padding(.horizontal, 12)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(Theme.Palette.bgSecondary(scheme))
        )
        .overlay(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
        )
    }

    @ViewBuilder
    private var content: some View {
        if vm.apps.isEmpty {
            EmptyLibraryView(onAdd: { showAdd = true })
        } else if vm.filteredApps.isEmpty {
            noMatchesView
        } else {
            ScrollView {
                LazyVGrid(columns: columns, alignment: .leading, spacing: Theme.Spacing.l) {
                    ForEach(vm.filteredApps) { app in
                        AppCardView(
                            app: app,
                            isRunning: runningIDs.contains(app.id),
                            onRun: { runApp(app) },
                            onOpenDetail: { detailApp = app }
                        )
                        .contextMenu {
                            Button("Run") { runApp(app) }
                            Button("Edit") { editingApp = app }
                            Divider()
                            Button("Delete", role: .destructive) { vm.delete(app) }
                        }
                    }
                }
                .padding(.horizontal, Theme.Spacing.xxl)
                .padding(.vertical, Theme.Spacing.xl)
            }
        }
    }

    private var noMatchesView: some View {
        VStack(spacing: Theme.Spacing.m) {
            Image(systemName: "magnifyingglass")
                .font(.system(size: 28, weight: .light))
                .foregroundColor(Theme.Palette.textTertiary(scheme))
            Text("No matches")
                .font(Theme.Font.heading)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
            Text("Nothing in your library matches “\(vm.search)”.")
                .font(Theme.Font.body)
                .foregroundColor(Theme.Palette.textSecondary(scheme))
            Button("Clear search") { vm.search = "" }
                .buttonStyle(.minimalGhost)
                .padding(.top, Theme.Spacing.s)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private var libraryCountLabel: String {
        let total = vm.apps.count
        let visible = vm.filteredApps.count
        if vm.search.isEmpty {
            return total == 1 ? "1 app" : "\(total) apps"
        }
        return "\(visible) of \(total)"
    }

    private func runApp(_ app: AppEntry) {
        guard !runningIDs.contains(app.id) else { return }
        runningIDs.insert(app.id)
        let runner = RunAppViewModel(settings: settingsVM.settings)
        let libraryVM = vm
        runner.onComplete = { result in
            Task { @MainActor in
                var updated = app
                updated.lastRunStatus = result?.status
                updated.lastDurationMs = result?.durationMs
                updated.updatedAt = Date()
                libraryVM.update(updated)
                runningIDs.remove(app.id)
            }
        }
        runner.run(app: app)
    }
}
