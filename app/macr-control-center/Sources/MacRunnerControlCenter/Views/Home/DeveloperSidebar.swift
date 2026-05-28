import SwiftUI

enum DeveloperPane: String, Hashable, CaseIterable {
    case library, runPanel
    case bottles, d3d, queue, blocks
    case doctor, logs, processes, performance
    case compatDB, corpus, trial
    case packaging, debugBundle, releases, worlds
    case help, settings

    var title: String {
        switch self {
        case .library: return "Library"
        case .runPanel: return "Quick Run"
        case .bottles: return "Bottles"
        case .d3d: return "D3D Artifacts"
        case .queue: return "Task Queue"
        case .blocks: return "Integration Blocks"
        case .doctor: return "Doctor"
        case .logs: return "Live Logs"
        case .processes: return "Wine Processes"
        case .performance: return "Performance"
        case .compatDB: return "Compatibility DB"
        case .corpus: return "Corpus"
        case .trial: return "Trial Wizard"
        case .packaging: return "Packaging"
        case .debugBundle: return "Debug Bundle"
        case .releases: return "Releases"
        case .worlds: return "Worlds"
        case .help: return "Help"
        case .settings: return "Settings"
        }
    }

    var symbol: String {
        switch self {
        case .library: return "square.stack.3d.up"
        case .runPanel: return "play.fill"
        case .bottles: return "archivebox"
        case .d3d: return "cube"
        case .queue: return "list.bullet.rectangle"
        case .blocks: return "square.grid.3x3"
        case .doctor: return "stethoscope"
        case .logs: return "doc.text.magnifyingglass"
        case .processes: return "cpu"
        case .performance: return "chart.bar"
        case .compatDB: return "checkmark.seal"
        case .corpus: return "doc.text"
        case .trial: return "sparkles"
        case .packaging: return "shippingbox"
        case .debugBundle: return "doc.zipper"
        case .releases: return "tag"
        case .worlds: return "globe"
        case .help: return "questionmark.circle"
        case .settings: return "gearshape"
        }
    }
}

struct DeveloperCategory {
    let label: String
    let items: [DeveloperPane]
}

let developerCategories: [DeveloperCategory] = [
    DeveloperCategory(label: "Apps", items: [.library, .runPanel]),
    DeveloperCategory(label: "Engine", items: [.bottles, .d3d, .queue, .blocks]),
    DeveloperCategory(label: "Diagnostics", items: [.doctor, .logs, .processes, .performance]),
    DeveloperCategory(label: "Catalog", items: [.compatDB, .corpus, .trial]),
    DeveloperCategory(label: "Release", items: [.packaging, .debugBundle, .releases, .worlds]),
    DeveloperCategory(label: "System", items: [.help, .settings])
]

struct DeveloperSidebarView: View {
    @Environment(\.colorScheme) private var scheme
    @Binding var selected: DeveloperPane
    @Binding var developerMode: Bool

    var body: some View {
        VStack(spacing: 0) {
            sidebarHeader
            Divider().background(Theme.Palette.separator(scheme))
            ScrollView {
                VStack(alignment: .leading, spacing: Theme.Spacing.xl) {
                    ForEach(developerCategories.indices, id: \.self) { idx in
                        let cat = developerCategories[idx]
                        VStack(alignment: .leading, spacing: 4) {
                            Text(cat.label.uppercased())
                                .font(Theme.Font.monoCaption)
                                .foregroundColor(Theme.Palette.textTertiary(scheme))
                                .kerning(1)
                                .padding(.horizontal, Theme.Spacing.m)
                                .padding(.bottom, 4)
                            ForEach(cat.items, id: \.self) { item in
                                sidebarRow(item)
                            }
                        }
                    }
                }
                .padding(.vertical, Theme.Spacing.l)
            }
            Divider().background(Theme.Palette.separator(scheme))
            exitButton
        }
        .frame(width: 240)
        .background(Theme.Palette.bgSecondary(scheme))
    }

    private var sidebarHeader: some View {
        HStack(spacing: 8) {
            Image(systemName: "hammer.fill")
                .font(.system(size: 11))
                .foregroundColor(Theme.Palette.textSecondary(scheme))
            VStack(alignment: .leading, spacing: 0) {
                Text("MacRunner")
                    .font(.system(size: 13, weight: .semibold))
                    .foregroundColor(Theme.Palette.textPrimary(scheme))
                Text("DEVELOPER MODE")
                    .font(Theme.Font.monoCaption)
                    .kerning(1)
                    .foregroundColor(Theme.Palette.textTertiary(scheme))
            }
            Spacer()
        }
        .padding(.horizontal, Theme.Spacing.l)
        .padding(.vertical, Theme.Spacing.m)
    }

    private func sidebarRow(_ item: DeveloperPane) -> some View {
        let isSelected = selected == item
        return Button {
            selected = item
        } label: {
            HStack(spacing: 10) {
                Image(systemName: item.symbol)
                    .font(.system(size: 12, weight: .regular))
                    .frame(width: 16)
                    .foregroundColor(
                        isSelected
                            ? Theme.Palette.onEmphasis(scheme)
                            : Theme.Palette.textSecondary(scheme)
                    )
                Text(item.title)
                    .font(Theme.Font.bodyEmph)
                    .foregroundColor(
                        isSelected
                            ? Theme.Palette.onEmphasis(scheme)
                            : Theme.Palette.textPrimary(scheme)
                    )
                Spacer()
            }
            .padding(.horizontal, Theme.Spacing.m)
            .padding(.vertical, 7)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.small, style: .continuous)
                    .fill(isSelected ? Theme.Palette.emphasis(scheme) : Color.clear)
            )
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .padding(.horizontal, Theme.Spacing.s)
    }

    private var exitButton: some View {
        Button {
            developerMode = false
        } label: {
            HStack {
                Image(systemName: "arrow.left")
                    .font(.system(size: 11))
                Text("Exit Developer Mode")
                    .font(Theme.Font.bodyEmph)
                Spacer()
            }
            .foregroundColor(Theme.Palette.textSecondary(scheme))
            .padding(.horizontal, Theme.Spacing.m)
            .padding(.vertical, 10)
        }
        .buttonStyle(.plain)
        .keyboardShortcut("d", modifiers: [.command, .shift])
    }
}

struct DeveloperPaneContainer<Content: View>: View {
    @Environment(\.colorScheme) private var scheme
    let title: String
    let symbol: String
    @ViewBuilder let content: () -> Content

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: symbol)
                    .font(.system(size: 14, weight: .regular))
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
                Text(title)
                    .font(Theme.Font.title)
                    .foregroundColor(Theme.Palette.textPrimary(scheme))
                Spacer()
            }
            .padding(.horizontal, Theme.Spacing.xl)
            .padding(.vertical, Theme.Spacing.l)
            Divider().background(Theme.Palette.separator(scheme))
            content()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Theme.Palette.bgPrimary(scheme))
    }
}
