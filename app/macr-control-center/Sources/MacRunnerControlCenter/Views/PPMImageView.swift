import SwiftUI

struct PPMImageView: View {
    let image: NSImage?

    var body: some View {
        if let image = image {
            Image(nsImage: image)
                .resizable()
                .scaledToFit()
                .frame(maxWidth: .infinity, maxHeight: .infinity)
        } else {
            ContentUnavailableView("No PPM image", systemImage: "photo")
        }
    }
}
