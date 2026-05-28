import SwiftUI

struct AppCardView: View {
    @Environment(\.colorScheme) private var scheme
    let app: AppEntry
    let isRunning: Bool
    let onRun: () -> Void
    let onOpenDetail: () -> Void

    @State private var hovering = false

    var body: some View {
        Button(action: onOpenDetail) {
            VStack(alignment: .leading, spacing: Theme.Spacing.m) {
                HStack(alignment: .top, spacing: Theme.Spacing.m) {
                    AppIconView(app: app, size: 56)
                    Spacer(minLength: 0)
                    statusGlyph
                        .frame(width: 18, height: 18)
                }

                VStack(alignment: .leading, spacing: Theme.Spacing.xs) {
                    Text(app.name)
                        .font(Theme.Font.heading)
                        .foregroundColor(Theme.Palette.textPrimary(scheme))
                        .lineLimit(1)
                    Text(subtitle)
                        .font(Theme.Font.caption)
                        .foregroundColor(Theme.Palette.textTertiary(scheme))
                        .lineLimit(1)
                }

                Spacer(minLength: 0)

                HStack(spacing: Theme.Spacing.s) {
                    Button(action: onRun) {
                        HStack(spacing: 6) {
                            Image(systemName: isRunning ? "stop.fill" : "play.fill")
                                .font(.system(size: 10, weight: .bold))
                            Text(isRunning ? "Stop" : "Run")
                                .font(Theme.Font.bodyEmph)
                        }
                        .foregroundColor(Theme.Palette.onEmphasis(scheme))
                        .padding(.vertical, 6)
                        .padding(.horizontal, 14)
                        .background(Theme.Palette.emphasis(scheme))
                        .clipShape(Capsule())
                    }
                    .buttonStyle(.scalePress)

                    if let status = app.lastRunStatus {
                        Text(status)
                            .font(Theme.Font.monoCaption)
                            .foregroundColor(Theme.Palette.textSecondary(scheme))
                            .padding(.vertical, 4)
                            .padding(.horizontal, 8)
                            .overlay(
                                Capsule()
                                    .stroke(Theme.Palette.border(scheme), lineWidth: 0.5)
                            )
                    }
                }
            }
            .padding(Theme.Spacing.l)
            .frame(width: 280, height: 220, alignment: .topLeading)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.large, style: .continuous)
                    .fill(Theme.Palette.bgElevated(scheme))
            )
            .overlay(
                RoundedRectangle(cornerRadius: Theme.Radius.large, style: .continuous)
                    .stroke(
                        hovering
                            ? Theme.Palette.emphasis(scheme).opacity(scheme == .dark ? 0.45 : 0.85)
                            : Theme.Palette.border(scheme).opacity(0.6),
                        lineWidth: hovering ? 1.0 : 0.5
                    )
            )
            .shadow(
                color: Color.black.opacity(hovering ? (scheme == .dark ? 0.55 : 0.14) : 0),
                radius: hovering ? 20 : 0,
                x: 0,
                y: hovering ? 10 : 0
            )
            .scaleEffect(hovering ? 1.02 : 1.0)
        }
        .buttonStyle(.plain)
        .animation(Theme.Motion.gentle, value: hovering)
        .onHover { hovering = $0 }
    }

    private var subtitle: String {
        if isRunning { return "Running…" }
        if let ms = app.lastDurationMs {
            return "Last run · \(formatDuration(ms))"
        }
        let file = (app.exePath as NSString).lastPathComponent
        return file.isEmpty ? app.exePath : file
    }

    private func formatDuration(_ ms: Int) -> String {
        if ms < 1000 { return "\(ms)ms" }
        let s = Double(ms) / 1000.0
        if s < 60 { return String(format: "%.1fs", s) }
        let m = Int(s) / 60
        let r = Int(s) % 60
        return "\(m)m \(r)s"
    }

    @ViewBuilder
    private var statusGlyph: some View {
        switch app.lastRunStatus {
        case "PASS":
            ZStack {
                Circle().fill(Theme.Palette.emphasis(scheme))
                Image(systemName: "checkmark")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundColor(Theme.Palette.onEmphasis(scheme))
            }
            .frame(width: 16, height: 16)
        case "FAIL", "CRASH":
            ZStack {
                Circle()
                    .stroke(Theme.Palette.textPrimary(scheme), lineWidth: 1)
                Image(systemName: "xmark")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundColor(Theme.Palette.textPrimary(scheme))
            }
            .frame(width: 16, height: 16)
        case "TIMEOUT":
            ZStack {
                Circle()
                    .stroke(Theme.Palette.textSecondary(scheme), lineWidth: 1)
                Image(systemName: "clock")
                    .font(.system(size: 9, weight: .bold))
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
            }
            .frame(width: 16, height: 16)
        default:
            EmptyView()
        }
    }
}
