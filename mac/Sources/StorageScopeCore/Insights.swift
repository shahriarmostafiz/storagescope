import Foundation

/// Large Files and Old Files: read the scan in memory, no disk access.
public enum Insights {
    /// Files of at least `minSize`, biggest first. `skip` returns true for folders to leave out (system software, app data).
    public static func largeFiles(root: Node, minSize: UInt64, category: FileCategory? = nil, skip: (Node) -> Bool = { _ in false },
                                  limit: Int = 5000) -> [Node] {
        var out: [Node] = []
        if skip(root) { return out }
        func walk(_ n: Node) {
            for c in n.children where !c.removed {
                if c.isDir {
                    if c.size >= minSize && !skip(c) { walk(c) }  // a folder smaller than the limit cannot hold a file above it
                } else if c.size >= minSize && (category == nil || FileCategory.of(name: c.name) == category!) {
                    out.append(c)
                }
            }
        }
        walk(root)
        out.sort { $0.size > $1.size }
        return Array(out.prefix(limit))
    }

    /// Files and whole folders not modified since `cutoff`. A folder whose newest file is older than the cutoff is listed as ONE row
    /// (not one row per file inside it). Only items of at least `minSize` are listed.
    public static func oldItems(root: Node, cutoff: Double, minSize: UInt64, skip: (Node) -> Bool = { _ in false }, limit: Int = 5000) -> [Node] {
        if skip(root) { return [] }
        // returns the newest modification time inside `n`, and fills `items` with what should be listed from it
        func walk(_ n: Node, _ items: inout [Node]) -> Double {
            if !n.isDir {
                if n.modified < cutoff && n.size >= minSize { items.append(n) }
                return n.modified
            }
            var newest = 0.0
            var mine: [Node] = []
            var any = false
            for c in n.children where !c.removed {
                if c.isDir && skip(c) { continue }
                any = true
                newest = max(newest, walk(c, &mine))
            }
            if !any { newest = n.modified }  // an empty folder counts by its own time
            if n.parent != nil && newest < cutoff && n.size >= minSize && n.files > 0 {
                items.append(n)  // everything inside is old: one row for the folder
            } else {
                items.append(contentsOf: mine)
            }
            return newest
        }
        var items: [Node] = []
        _ = walk(root, &items)
        items.sort { $0.size > $1.size }
        return Array(items.prefix(limit))
    }
}
