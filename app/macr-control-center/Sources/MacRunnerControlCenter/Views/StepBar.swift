import SwiftUI

struct StepBar: View {
    let steps: [String]
    let current: Int

    var body: some View {
        HStack {
            ForEach(steps.indices, id: \.self) { i in
                HStack(spacing: 4) {
                    Circle()
                        .fill(i <= current ? Color.accentColor : Color.secondary.opacity(0.3))
                        .frame(width: 10, height: 10)
                    Text(steps[i])
                        .font(.caption)
                        .foregroundStyle(i <= current ? .primary : .secondary)
                }
                if i < steps.count - 1 {
                    Spacer()
                    Rectangle()
                        .fill(i < current ? Color.accentColor : Color.secondary.opacity(0.3))
                        .frame(height: 1)
                }
            }
        }
    }
}
