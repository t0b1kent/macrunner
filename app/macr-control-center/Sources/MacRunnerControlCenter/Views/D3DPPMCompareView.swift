import SwiftUI

struct D3DPPMCompareView: View {
    let leftPath: String?
    let rightPath: String?

    var body: some View {
        HStack(spacing: 12) {
            VStack {
                Text("Left").font(.caption)
                PPMPreview(path: leftPath)
            }
            VStack {
                Text("Right").font(.caption)
                PPMPreview(path: rightPath)
            }
        }
        .padding()
    }
}

struct PPMPreview: View {
    let path: String?
    var body: some View {
        if let path = path, let data = try? Data(contentsOf: URL(fileURLWithPath: path)), let image = PPMParser.parse(data: data) {
            Image(nsImage: image)
                .resizable()
                .scaledToFit()
                .frame(maxWidth: 300, maxHeight: 300)
        } else {
            Rectangle()
                .fill(Color(.controlBackgroundColor))
                .frame(width: 200, height: 200)
                .overlay(Text("No image").foregroundStyle(.secondary))
        }
    }
}
