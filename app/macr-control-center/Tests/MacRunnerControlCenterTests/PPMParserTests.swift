import Foundation
import Testing
@testable import MacRunnerControlCenter

struct PPMParserTests {
    @Test func parseInvalidGracefully() {
        let data = Data("not a ppm".utf8)
        let image = PPMParser.parse(data: data)
        #expect(image == nil)
    }

    @Test func parseValidP6() {
        var header = "P6\n2 2\n255\n"
        var pixels: [UInt8] = [
            255, 0, 0,   0, 255, 0,
            0, 0, 255,   255, 255, 255
        ]
        var data = Data(header.utf8)
        data.append(contentsOf: pixels)
        let image = PPMParser.parse(data: data)
        #expect(image != nil)
    }
}
