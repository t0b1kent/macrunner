import Foundation
import Testing
@testable import MacRunnerControlCenter

struct PathSafetyTests {
    @Test func subpathCheck() {
        let parent = "/Users/timurtoby/Library/Application Support/MacRunner/bottles"
        let child = "/Users/timurtoby/Library/Application Support/MacRunner/bottles/mybottle"
        let outside = "/Users/timurtoby/Desktop"
        #expect(PathSafety.isSubpath(of: parent, path: child) == true)
        #expect(PathSafety.isSubpath(of: parent, path: outside) == false)
    }

    @Test func sanitizeBottle() {
        let base = "/Users/timurtoby/Library/Application Support/MacRunner/bottles"
        let valid = "/Users/timurtoby/Library/Application Support/MacRunner/bottles/a"
        let invalid = "/Users/timurtoby/Desktop/a"
        #expect(PathSafety.sanitizeBottlePath(valid, base: base) == true)
        #expect(PathSafety.sanitizeBottlePath(invalid, base: base) == false)
    }

    @Test func identicalPathIsSubpath() {
        let path = "/Users/timurtoby/Library/Application Support/MacRunner/bottles"
        #expect(PathSafety.isSubpath(of: path, path: path) == true)
    }
}
