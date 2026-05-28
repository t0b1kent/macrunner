import SwiftUI

struct AppIconView: View {
    @Environment(\.colorScheme) private var scheme
    let app: AppEntry
    let size: CGFloat

    var body: some View {
        ZStack {
            RoundedRectangle(cornerRadius: size * 0.22, style: .continuous)
                .fill(
                    LinearGradient(
                        colors: [
                            Theme.Palette.bgTertiary(scheme),
                            scheme == .dark ? Color(white: 0.08) : Color(white: 0.92)
                        ],
                        startPoint: .topLeading,
                        endPoint: .bottomTrailing
                    )
                )
            RoundedRectangle(cornerRadius: size * 0.22, style: .continuous)
                .strokeBorder(
                    LinearGradient(
                        colors: [
                            Theme.Palette.border(scheme).opacity(scheme == .dark ? 0.9 : 0.7),
                            Theme.Palette.border(scheme).opacity(0.2)
                        ],
                        startPoint: .top,
                        endPoint: .bottom
                    ),
                    lineWidth: 0.5
                )
            Text(initials)
                .font(.system(size: size * 0.42, weight: .semibold, design: .default))
                .foregroundColor(Theme.Palette.textPrimary(scheme))
                .kerning(-0.5)
        }
        .frame(width: size, height: size)
    }

    private var initials: String {
        let words = app.name
            .split(separator: " ")
            .prefix(2)
            .compactMap { $0.first }
            .map { String($0).uppercased() }
        if words.isEmpty {
            return "·"
        }
        return words.joined()
    }
}
