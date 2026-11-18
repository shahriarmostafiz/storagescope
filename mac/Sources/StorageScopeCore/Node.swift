import Foundation

/// One file or folder of a scan. The tree owns its children; `parent` is not retained (the root outlives the tree).
public final class Node: Identifiable {
    public let name: String            // the scan root stores its full path, everything else just its own name
    public unowned(unsafe) var parent: Node?
    public let isDir: Bool
    public var size: UInt64            // allocated bytes (folders: everything below)
    public var files: Int              // number of files (a file counts 1)
    public var modified: Double        // seconds since 1970
    public var children: [Node] = []
    public var removed = false
    public var unreadable = false      // the folder could not be opened (usually missing Full Disk Access)

    /// While a scan runs, scanner threads add to the totals and publish `children`; everything that browses a LIVE scan reads through the
    /// accessors below (`kids`, `liveSize`, `liveFiles`), which take this lock. After a scan nothing writes, and plain reads are fine too.
    public static let lock = NSLock()
    /// True from the moment a running scan creates this folder until its listing has been published into `children`.
    public var building = false

    public init(name: String, isDir: Bool, size: UInt64 = 0, files: Int = 0, modified: Double = 0, parent: Node? = nil) {
        self.name = name
        self.isDir = isDir
        self.size = size
        self.files = files
        self.modified = modified
        self.parent = parent
    }

    public var id: ObjectIdentifier { ObjectIdentifier(self) }

    /// Full path, built by walking up (not stored: it would cost memory for every node of a big scan).
    public var path: String {
        var parts: [String] = []
        var n: Node = self
        while let p = n.parent {
            parts.append(n.name)
            n = p
        }
        if parts.isEmpty { return n.name }
        let base = n.name == "/" ? "" : n.name
        return base + parts.reversed().map { "/" + $0 }.joined()
    }

    /// Path of the folder this item sits in.
    public var location: String { parent?.path ?? "" }

    /// The children that are safe to read while a scan is running: nothing until the folder's listing is in.
    public var kids: [Node] {
        Node.lock.lock()
        defer { Node.lock.unlock() }
        return building ? [] : children
    }

    public var liveSize: UInt64 {
        Node.lock.lock()
        defer { Node.lock.unlock() }
        return size
    }

    public var liveFiles: Int {
        Node.lock.lock()
        defer { Node.lock.unlock() }
        return files
    }

    public var isBuilding: Bool {
        Node.lock.lock()
        defer { Node.lock.unlock() }
        return building
    }

    /// Largest children first (removed ones left out).
    public func childrenBySize(limit: Int = Int.max) -> [Node] {
        let live = kids.filter { !$0.removed }.map { ($0, $0.liveSize) }
        return Array(live.sorted { $0.1 > $1.1 }.prefix(limit).map { $0.0 })
    }

    /// Looks a path up below this node (call it on the scan root). Case-insensitive, like the default Mac volume.
    public func find(path target: String) -> Node? {
        let me = path
        if target == me { return self }
        let prefix = me == "/" ? "/" : me + "/"
        guard target.hasPrefix(prefix) else { return nil }
        var cur: Node = self
        for comp in target.dropFirst(prefix.count).split(separator: "/") {
            let c = String(comp)
            guard let next = cur.kids.first(where: { !$0.removed && $0.name.compare(c, options: .caseInsensitive) == .orderedSame }) else { return nil }
            cur = next
        }
        return cur
    }

    /// Takes this item out of the tree after it was moved to the Trash and corrects the totals above it.
    public func detach() {
        guard !removed else { return }
        removed = true
        var p = parent
        while let q = p {
            q.size = q.size >= size ? q.size - size : 0
            q.files = max(0, q.files - files)
            p = q.parent
        }
        parent?.children.removeAll { $0 === self }
    }

    /// Drops every node that has a selected ancestor (trashing the folder already takes what is inside it).
    public static func topLevel(_ nodes: [Node]) -> [Node] {
        let ids = Set(nodes.map { $0.id })
        return nodes.filter { n in
            var p = n.parent
            while let q = p {
                if ids.contains(q.id) { return false }
                p = q.parent
            }
            return true
        }
    }
}
