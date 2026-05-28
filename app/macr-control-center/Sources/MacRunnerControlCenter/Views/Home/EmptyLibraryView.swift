import SwiftUI

struct EmptyLibraryView: View {
    @Environment(\.colorScheme) private var scheme
    let onAdd: () -> Void

    @State private var pulse = false

    var body: some View {
        VStack(spacing: Theme.Spacing.xxl) {
            ZStack {
                ForEach(0..<3) { i in
                    Circle()
                        .stroke(
                            Theme.Palette.separator(scheme),
                            style: StrokeStyle(lineWidth: 0.5, dash: [3, 3])
                        )
                        .frame(width: CGFloat(160 + i * 60), height: CGFloat(160 + i * 60))
                        .opacity(pulse ? 0.0 : Double(0.7 - Double(i) * 0.2))
                        .scaleEffect(pulse ? 1.05 : 1.0)
                        .animation(
                            .easeInOut(duration: 2.0).repeatForever(autoreverses: true).delay(Double(i) * 0.3),
                            value: pulse
                        )
                }
                Image(systemName: "square.stack.3d.up")
                    .font(.system(size: 40, weight: .light))
                    .foregroundColor(Theme.Palette.textTertiary(scheme))
            }
            .frame(height: 280)

            VStack(spacing: Theme.Spacing.s) {
                Text("Your library is empty")
                    .font(Theme.Font.title)
                    .foregroundColor(Theme.Palette.textPrimary(scheme))
                Text("Drop a Windows app or browse to add one.")
                    .font(Theme.Font.body)
                    .foregroundColor(Theme.Palette.textSecondary(scheme))
            }

            Button("Add App", action: onAdd)
                .buttonStyle(.minimalPrimary)
                .keyboardShortcut("n", modifiers: .command)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .onAppear { pulse = true }
    }
}
