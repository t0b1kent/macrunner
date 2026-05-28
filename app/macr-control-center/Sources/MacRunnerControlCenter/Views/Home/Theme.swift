import SwiftUI

enum Theme {
    enum Palette {
        static let bg = Color(nsColor: .windowBackgroundColor)
        static let surface = Color(nsColor: .controlBackgroundColor)

        static func bgPrimary(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.04) : Color.white
        }

        static func bgSecondary(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.08) : Color(white: 0.98)
        }

        static func bgTertiary(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.12) : Color(white: 0.96)
        }

        static func bgElevated(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.16) : Color.white
        }

        static func separator(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.18) : Color(white: 0.90)
        }

        static func border(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.22) : Color(white: 0.86)
        }

        static func textPrimary(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.96) : Color(white: 0.04)
        }

        static func textSecondary(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.70) : Color(white: 0.36)
        }

        static func textTertiary(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color(white: 0.50) : Color(white: 0.58)
        }

        static func emphasis(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color.white : Color.black
        }

        static func onEmphasis(_ scheme: ColorScheme) -> Color {
            scheme == .dark ? Color.black : Color.white
        }
    }

    enum Spacing {
        static let xs: CGFloat = 4
        static let s: CGFloat = 8
        static let m: CGFloat = 12
        static let l: CGFloat = 16
        static let xl: CGFloat = 24
        static let xxl: CGFloat = 32
        static let xxxl: CGFloat = 48
        static let huge: CGFloat = 64
    }

    enum Radius {
        static let small: CGFloat = 6
        static let medium: CGFloat = 10
        static let large: CGFloat = 14
        static let pill: CGFloat = 999
    }

    enum Motion {
        static let fast: Animation = .easeOut(duration: 0.12)
        static let standard: Animation = .easeOut(duration: 0.20)
        static let gentle: Animation = .spring(response: 0.42, dampingFraction: 0.86)
    }

    enum Font {
        static let displayLarge = SwiftUI.Font.system(size: 44, weight: .semibold, design: .default)
        static let display = SwiftUI.Font.system(size: 32, weight: .semibold, design: .default)
        static let title = SwiftUI.Font.system(size: 22, weight: .semibold, design: .default)
        static let heading = SwiftUI.Font.system(size: 17, weight: .semibold, design: .default)
        static let body = SwiftUI.Font.system(size: 13, weight: .regular, design: .default)
        static let bodyEmph = SwiftUI.Font.system(size: 13, weight: .medium, design: .default)
        static let caption = SwiftUI.Font.system(size: 11, weight: .regular, design: .default)
        static let mono = SwiftUI.Font.system(size: 12, weight: .regular, design: .monospaced)
        static let monoCaption = SwiftUI.Font.system(size: 10, weight: .regular, design: .monospaced)
    }
}

struct ScalePressStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .scaleEffect(configuration.isPressed ? 0.97 : 1.0)
            .opacity(configuration.isPressed ? 0.85 : 1.0)
            .animation(Theme.Motion.fast, value: configuration.isPressed)
    }
}

extension ButtonStyle where Self == ScalePressStyle {
    static var scalePress: ScalePressStyle { ScalePressStyle() }
}

struct MinimalPrimaryButtonStyle: ButtonStyle {
    @Environment(\.colorScheme) private var scheme

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Theme.Font.bodyEmph)
            .foregroundColor(Theme.Palette.onEmphasis(scheme))
            .padding(.vertical, 10)
            .padding(.horizontal, 20)
            .background(Theme.Palette.emphasis(scheme))
            .clipShape(RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous))
            .scaleEffect(configuration.isPressed ? 0.98 : 1.0)
            .opacity(configuration.isPressed ? 0.88 : 1.0)
            .animation(Theme.Motion.fast, value: configuration.isPressed)
    }
}

extension ButtonStyle where Self == MinimalPrimaryButtonStyle {
    static var minimalPrimary: MinimalPrimaryButtonStyle { MinimalPrimaryButtonStyle() }
}

struct MinimalSecondaryButtonStyle: ButtonStyle {
    @Environment(\.colorScheme) private var scheme

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Theme.Font.bodyEmph)
            .foregroundColor(Theme.Palette.textPrimary(scheme))
            .padding(.vertical, 10)
            .padding(.horizontal, 20)
            .overlay(
                RoundedRectangle(cornerRadius: Theme.Radius.medium, style: .continuous)
                    .stroke(Theme.Palette.border(scheme), lineWidth: 1)
            )
            .scaleEffect(configuration.isPressed ? 0.98 : 1.0)
            .opacity(configuration.isPressed ? 0.88 : 1.0)
            .animation(Theme.Motion.fast, value: configuration.isPressed)
    }
}

extension ButtonStyle where Self == MinimalSecondaryButtonStyle {
    static var minimalSecondary: MinimalSecondaryButtonStyle { MinimalSecondaryButtonStyle() }
}

struct MinimalGhostButtonStyle: ButtonStyle {
    @Environment(\.colorScheme) private var scheme

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Theme.Font.bodyEmph)
            .foregroundColor(Theme.Palette.textSecondary(scheme))
            .padding(.vertical, 6)
            .padding(.horizontal, 10)
            .background(
                RoundedRectangle(cornerRadius: Theme.Radius.small, style: .continuous)
                    .fill(configuration.isPressed ? Theme.Palette.bgTertiary(scheme) : Color.clear)
            )
            .animation(Theme.Motion.fast, value: configuration.isPressed)
    }
}

extension ButtonStyle where Self == MinimalGhostButtonStyle {
    static var minimalGhost: MinimalGhostButtonStyle { MinimalGhostButtonStyle() }
}
