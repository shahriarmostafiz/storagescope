import CryptoKit
import Darwin
import Foundation

/// Finds files with exactly the same content: same size first, then a hash of the start and end of the file, then a hash of the
/// whole file. Only files that look alike are ever read in full.
public enum DuplicateFinder {
    private static let edge: UInt64 = 64 * 1024  // the quick check reads this much from the start and from the end
    private static let dataless: UInt32 = 0x4000_0000  // SF_DATALESS: an iCloud file that is not on this Mac; reading it would download it

    /// A file of a group, with what the search saw of it. `stamp` (the modification time in nanoseconds) lets the caller notice that
    /// the file changed afterwards.
    public struct Hit {
        public let node: Node
        public let path: String
        public let size: UInt64
        public let stamp: Int64
    }

    /// Live progress, read by the UI while `find` runs on another thread.
    public final class Progress: @unchecked Sendable {
        private let lock = NSLock()
        private var stage = 0, done = 0, total = 0, skipped = 0
        public init() {}

        func begin(stage: Int, total: Int) {
            lock.lock(); defer { lock.unlock() }
            self.stage = stage; self.done = 0; self.total = total
        }

        func tick() {
            lock.lock(); defer { lock.unlock() }
            done += 1
        }

        func skip(_ n: Int) {
            lock.lock(); defer { lock.unlock() }
            skipped += n
        }

        /// stage 0 = sorting by size, 1 = quick check, 2 = full comparison. `skipped` = files that could not be checked
        /// (in use, a link, an iCloud file that is not downloaded, changed while read).
        public var snapshot: (stage: Int, done: Int, total: Int, skipped: Int) {
            lock.lock(); defer { lock.unlock() }
            return (stage, min(done, total), total, skipped)
        }
    }

    /// Set from any thread to make `find` stop early.
    public final class Cancel: @unchecked Sendable {
        private let lock = NSLock()
        private var flag = false
        public init() {}
        public func cancel() { lock.lock(); flag = true; lock.unlock() }
        public var isCancelled: Bool { lock.lock(); defer { lock.unlock() }; return flag }
    }

    /// What was seen of one file: its identity (device and inode) and its modification time.
    private struct Seen {
        var dev: Int32 = 0
        var ino: UInt64 = 0
        var stamp: Int64 = 0
    }

    private static func stampOf(_ st: stat) -> Int64 { Int64(st.st_mtimespec.tv_sec) * 1_000_000_000 + Int64(st.st_mtimespec.tv_nsec) }

    /// Files of at least `minSize` in tree order (the order folders sit on the disk). `skip` returns true for folders to leave out.
    public static func candidates(root: Node, minSize: UInt64, category: FileCategory? = nil, skip: (Node) -> Bool = { _ in false }) -> [Node] {
        var out: [Node] = []
        if skip(root) { return out }
        func walk(_ n: Node) {
            for c in n.children where !c.removed {
                if c.isDir {
                    // repository internals are full of look-alike objects, and removing one can corrupt the repository
                    if c.size >= minSize && !skip(c) && ![".git", ".svn", ".hg"].contains(c.name) { walk(c) }
                } else if c.size >= minSize && (category == nil || FileCategory.of(name: c.name) == category!) {
                    out.append(c)
                }
            }
        }
        walk(root)
        return out
    }

    /// Groups of files with identical content, two or more each. The first file of a group is the one to keep (the oldest, then the
    /// shortest path). The groups with the most reclaimable space come first. Several names of one file (hard links) count as one file.
    /// Links, iCloud files that are not downloaded, and files that change while they are read are left out. Returns nothing when
    /// cancelled. `threads` > 1 suits solid-state storage.
    public static func find(_ files: [Node], threads: Int = 4, cancel: Cancel = Cancel(), progress: Progress? = nil) -> [[Hit]] {
        progress?.begin(stage: 0, total: files.count)
        // The real size of each file (allocated size would miss a compressed copy of an identical file).
        var paths = [String](repeating: "", count: files.count)
        var sizes = [UInt64](repeating: 0, count: files.count)
        for i in 0..<files.count {
            if cancel.isCancelled { return [] }
            let p = files[i].path
            var st = stat()
            if lstat(p, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG, st.st_flags & dataless == 0 {
                if st.st_size > 0 {
                    paths[i] = p
                    sizes[i] = UInt64(st.st_size)
                }
            } else {
                progress?.skip(1)
            }
            progress?.tick()
        }
        var bySize: [UInt64: [Int]] = [:]
        for i in 0..<files.count where sizes[i] > 0 { bySize[sizes[i], default: []].append(i) }
        let same = bySize.values.filter { $0.count > 1 }
        var seen = [Seen](repeating: Seen(), count: files.count)

        let quick = refine(same, paths: paths, sizes: sizes, edges: true, stage: 1, threads: threads, cancel: cancel, progress: progress, seen: &seen)
        // a file of at most 128 KB was read completely by the quick check, so its group is already final
        var finals = quick.filter { sizes[$0[0]] <= 2 * edge }
        let big = quick.filter { sizes[$0[0]] > 2 * edge }
        if !big.isEmpty && !cancel.isCancelled {
            finals += refine(big, paths: paths, sizes: sizes, edges: false, stage: 2, threads: threads, cancel: cancel, progress: progress, seen: &seen)
        }
        if cancel.isCancelled { return [] }

        func ordered(_ g: [Int]) -> [Int] {  // the file to keep comes first
            let sorted = g.sorted { a, b in
                let x = files[a], y = files[b]
                if x.modified != y.modified { return x.modified < y.modified }
                let px = paths[a], py = paths[b]
                return px.count != py.count ? px.count < py.count : px < py
            }
            var ids = Set<[UInt64]>()  // two names of one file are not copies: keep the first name only
            return sorted.filter { ids.insert([UInt64(bitPattern: Int64(seen[$0].dev)), seen[$0].ino]).inserted }
        }
        let groups = finals.map(ordered).filter { $0.count > 1 }.sorted { a, b in
            let wa = sizes[a[0]] * UInt64(a.count - 1), wb = sizes[b[0]] * UInt64(b.count - 1)
            return wa != wb ? wa > wb : paths[a[0]] < paths[b[0]]
        }
        return groups.map { $0.map { Hit(node: files[$0], path: paths[$0], size: sizes[$0], stamp: seen[$0].stamp) } }
    }

    /// Splits every group into the files whose digests are equal; only subgroups of two or more survive.
    /// Files are read in tree order, not group by group, so a hard disk does not jump between far-apart groups.
    private static func refine(_ groups: [[Int]], paths: [String], sizes: [UInt64], edges: Bool, stage: Int, threads: Int,
                               cancel: Cancel, progress: Progress?, seen: inout [Seen]) -> [[Int]] {
        var tasks: [(idx: Int, g: Int)] = []
        for (g, members) in groups.enumerated() { for i in members { tasks.append((i, g)) } }
        tasks.sort { $0.idx < $1.idx }
        progress?.begin(stage: stage, total: tasks.count)

        let lock = NSLock()
        var digests = [Data?](repeating: nil, count: tasks.count)
        var next = 0
        var local = seen
        DispatchQueue.concurrentPerform(iterations: max(1, min(threads, 8))) { _ in
            var buf = [UInt8](repeating: 0, count: 1 << 20)
            while true {
                lock.lock()
                let t = next
                next += 1
                lock.unlock()
                if t >= tasks.count || cancel.isCancelled { return }
                let i = tasks[t].idx
                var info = Seen()
                let d = digest(paths[i], size: sizes[i], edges: edges, buf: &buf, cancel: cancel, seen: &info)
                lock.lock()
                digests[t] = d
                if d != nil { local[i] = info }
                lock.unlock()
                progress?.tick()
            }
        }
        seen = local
        if cancel.isCancelled { return [] }
        progress?.skip(digests.filter { $0 == nil }.count)

        var by = [[Data: [Int]]](repeating: [:], count: groups.count)
        for (t, task) in tasks.enumerated() { if let d = digests[t] { by[task.g][d, default: []].append(task.idx) } }
        return by.flatMap { $0.values.filter { $0.count > 1 } }
    }

    /// SHA-256 of a whole file, or of its first and last 64 KB (the whole file when it is at most 128 KB).
    /// nil when the file cannot be read, is not a plain file of the size it had when it was listed, or changes while it is read.
    private static func digest(_ path: String, size: UInt64, edges: Bool, buf: inout [UInt8], cancel: Cancel, seen: inout Seen) -> Data? {
        let fd = open(path, O_RDONLY | O_NOFOLLOW)  // a path that has become a link is not followed
        if fd < 0 { return nil }
        defer { close(fd) }
        var before = stat()
        guard fstat(fd, &before) == 0, (before.st_mode & S_IFMT) == S_IFREG, before.st_flags & dataless == 0, UInt64(before.st_size) == size else { return nil }
        var hasher = SHA256()
        func feed(_ offset: UInt64, _ count: UInt64) -> Bool {
            var off = off_t(offset)
            var left = count
            while left > 0 {
                if cancel.isCancelled { return false }
                let want = Int(min(left, UInt64(buf.count)))
                let got = buf.withUnsafeMutableBytes { pread(fd, $0.baseAddress, want, off) }
                if got != want { return false }  // short read: the file shrank
                buf.withUnsafeBytes { hasher.update(bufferPointer: UnsafeRawBufferPointer(rebasing: $0.prefix(got))) }
                off += off_t(got)
                left -= UInt64(got)
            }
            return true
        }
        if edges {
            guard feed(0, min(size, edge)) else { return nil }
            if size > edge {
                let tail = min(edge, size - edge)
                guard feed(size - tail, tail) else { return nil }
            }
        } else {
            guard feed(0, size) else { return nil }
        }
        var after = stat()
        // a write during the read gives a hash of a mix of old and new content
        guard fstat(fd, &after) == 0, stampOf(after) == stampOf(before), after.st_size == before.st_size else { return nil }
        seen = Seen(dev: before.st_dev, ino: UInt64(before.st_ino), stamp: stampOf(before))
        return Data(hasher.finalize())
    }
}
