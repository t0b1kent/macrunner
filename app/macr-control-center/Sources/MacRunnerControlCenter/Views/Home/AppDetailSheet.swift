import SwiftUI

struct AppDetailSheet: View {
    @Environment(\.colorScheme) private var scheme
    let app: AppEntry
    let isRunning: Bool
    let onRun: () -> Void
    let onEdit: () -> Void
    let onDelete: () -> Void
    let onClose: () -> Void

    @State private var confirmingDelete = false

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider().background(Theme.Palette.separator(scheme))
            content
            Divider().background(Theme.Palette.separator(scheme))
            footer
        }
        .frame(width: 520)
        .background(Theme.Palette.bgPrimary(scheme))
    }

    private var header: some View {
        HStack(alignment: .top, spacing: Theme.Spacing.l) {
            AppIconView(app: app, size: 64)
            VStack(alignment: .leading, spacing: 4) {
                Text(app.name)
                    .font(Theme.Font.title)
                    .foregroundColor(Theme.Palette.textPrimary(scheme))
                    .lineLimit(1)
                Text((app.exePath as NSString).lastPathComponent)
                    .font(Theme.Font.mono)
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
                    .lineLimit(1)
                Text(app.exePath)
                    .font(Theme.Font.caption)
                    .foregroundColor(Theme.Palette.textTertiary(scheme))
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            Button(action: onClose) {
                Image(systemName: "xmark")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
                    .frame(width: 28, height: 28)
                    .background(
                        Circle()
                            .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                    )
            }
            .buttonStyle(.scalePress)
            .keyboardShortcut(.escape, modifiers: [])
        }
        .padding(Theme.Spacing.xl)
    }

    private var content: some View {
        VStack(alignment: .leading, spacing: Theme.Spacing.l) {
            statsGrid
            if let notes = app.notes, !notes.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    Text("NOTES")
                        .font(Theme.Font.monoCaption)
                        .foregroundColor(Theme.Palette.textTertiary(scheme))
                        .kerning(1)
                    Text(notes)
                        .font(Theme.Font.body)
                        .foregroundColor(Theme.Palette.textPrimary(scheme))
                }
            }
            if let tags = app.tags, !tags.isEmpty {
                tagRow(tags)
            }
        }
        .padding(.horizontal, Theme.Spacing.xl)
        .padding(.vertical, Theme.Spacing.l)
    }

    private var statsGrid: some View {
        HStack(spacing: Theme.Spacing.m) {
            statTile(label: "STATUS", value: app.lastRunStatus ?? "—")
            statTile(label: "LAST RUN", value: lastDurationText)
            statTile(label: "BACKEND", value: app.d3dBackend.uppercased())
            statTile(label: "TIMEOUT", value: app.timeout.map { "\($0)s" } ?? "—")
        }
    }

    private func statTile(label: String, value: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(label)
                .font(Theme.Font.monoCaption)
                .foregroundColor(Theme.Palette.textTertiary(scheme))
                .kerning(1)
            Text(value)
                .font(Theme.Font.bodyEmph)
                .foregroundColor(Theme.Palette.textPrimary(scheme))
                .lineLimit(1)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(Theme.Spacing.m)
        .background(
            RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                .fill(Theme.Palette.bgSecondary(scheme))
        )
    }

    private func tagRow(_ tags: [String]) -> some View {
        HStack(spacing: 6) {
            ForEach(tags, id: \.self) { tag in
                Text(tag)
                    .font(Theme.Font.caption)
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
                    .padding(.vertical, 4)
                    .padding(.horizontal, 10)
                    .overlay(
                        Capsule()
                            .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                    )
            }
        }
    }

    private var footer: some View {
        HStack(spacing: Theme.Spacing.m) {
            if confirmingDelete {
                Text("Delete this app from library?")
                    .font(Theme.Font.body)
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
                Spacer()
                Button("Cancel") { confirmingDelete = false }
                    .buttonStyle(.minimalGhost)
                Button("Delete", role: .destructive) {
                    onDelete()
                }
                .buttonStyle(.minimalSecondary)
            } else {
                Button {
                    confirmingDelete = true
                } label: {
                    Image(systemName: "trash")
                        .font(.system(size: 13, weight: .regular))
                        .foregroundColor(Theme.Palette.textSecondary(scheme))
                }
                .buttonStyle(.minimalGhost)

                Spacer()

                Button("Edit", action: onEdit)
                    .buttonStyle(.minimalSecondary)

                Button(action: onRun) {
                    HStack(spacing: 6) {
                        Image(systemName: isRunning ? "stop.fill" : "play.fill")
                            .font(.system(size: 11, weight: .bold))
                        Text(isRunning ? "Stop" : "Run")
                    }
                }
                .buttonStyle(.minimalPrimary)
                .keyboardShortcut(.return, modifiers: [])
            }
        }
        .padding(Theme.Spacing.l)
    }

    private var lastDurationText: String {
        guard let ms = app.lastDurationMs else { return "—" }
        if ms < 1000 { return "\(ms)ms" }
        let s = Double(ms) / 1000.0
        if s < 60 { return String(format: "%.1fs", s) }
        return "\(Int(s) / 60)m \(Int(s) % 60)s"
    }
}
