import Foundation

extension String {
    var macrExpandingTilde: String { (self as NSString).expandingTildeInPath }
}
