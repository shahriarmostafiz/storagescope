import Foundation

public struct TrashFailure {
    public let path: String
    public let reason: String
}

public struct TrashResult {
    public var trashed: [String] = []
    public var failed: [TrashFailure] = []
    public init() {}
}

/// The only way this app removes anything: move to the Trash (restorable), never a permanent delete.
public enum Trasher {
    public static func trash(paths: [String], rules: ProtectionRules) -> TrashResult {
        var res = TrashResult()
        for p in paths {
            if rules.classify(p) == .blocked {
                res.failed.append(TrashFailure(path: p, reason: "Protected location"))
                continue
            }
            do {
                try FileManager.default.trashItem(at: URL(fileURLWithPath: p), resultingItemURL: nil)
                res.trashed.append(p)
            } catch {
                res.failed.append(TrashFailure(path: p, reason: error.localizedDescription))
            }
        }
        return res
    }
}
