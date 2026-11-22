import CryptoKit
import Darwin
import Foundation

/// Finds files with exactly the same content: same size first, then a hash of the start and end of the file, then a hash of the
/// whole file. Only files that look alike are ever read in full.
public enum DuplicateFinder {
    private static let edge: UInt64 = 64 * 1024  // the quick check reads this much from the start and from the end

    /// Live progress, read by the UI while `find` runs on another thread.
    public final class Progress: @unchecked Sendable {
        private let lock = NSLock()
        private var stage = 0, done = 0, total = 0
        public init() {}

        func begin(stage: Int, total: Int) {
            lock.lock(); defer { lock.unlock() }
            self.stage = stage; self.done = 0; self.total = total
        }

        func tick() {
            lock.lock(); defer { lock.unlock() }
            done += 1
        }

        /// stage 0 = sorting by size, 1 = quick check, 2 = full comparison
        public var snapshot: (stage: Int, done: Int, total: Int) {
            lock.lock(); defer { lock.unlock() }
            return (stage, min(done, total), total)
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

    /// Groups of nodes with identical content, two or more each. The first node of a group is the one to keep (the oldest, then the
    /// shortest path). The groups with the most reclaimable space come first. Returns nothing when cancelled.
    /// `threads` > 1 suits solid-state storage.
    public static func find(_ files: [Node], threads: Int = 4, cancel: Cancel = Cancel(), progress: Progress? = nil) -> [[Node]] {
        progress?.begin(stage: 0, total: files.count)
        // The real size of each file (allocated size would miss a compressed copy of an identical file).
        var paths = [String](repeating: "", count: files.count)
        var sizes = [UInt64](repeating: 0, count: files.count)
        for i in 0..<files.count {
            if cancel.isCancelled { return [] }
            let p = files[i].path
            var st = stat()
            if lstat(p, &st) == 0, (st.st_mode & S_IFMT) == S_IFREG, st.st_size > 0 {
                paths[i] = p
                sizes[i] = UInt64(st.st_size)
            }
            progress?.tick()
        }
        var bySize: [UInt64: [Int]] = [:]
        for i in 0..<files.count where sizes[i] > 0 { bySize[sizes[i], default: []].append(i) }
        let same = bySize.values.filter { $0.count > 1 }

        let quick = refine(same, paths: paths, sizes: sizes, edges: true, stage: 1, threads: threads, cancel: cancel, progress: progress)
        // a file of at most 128 KB was read completely by the quick check, so its group is already final
        var finals = quick.filter { sizes[$0[0]] <= 2 * edge }
        let big = quick.filter { sizes[$0[0]] > 2 * edge }
        if !big.isEmpty && !cancel.isCancelled {
            finals += refine(big, paths: paths, sizes: sizes, edges: false, stage: 2, threads: threads, cancel: cancel, progress: progress)
        }
        if cancel.isCancelled { return [] }

        func ordered(_ g: [Int]) -> [Int] {  // the file to keep comes first
            g.sorted { a, b in
                let x = files[a], y = files[b]
                if x.modified != y.modified { return x.modified < y.modified }
                let px = paths[a], py = paths[b]
                return px.count != py.count ? px.count < py.count : px < py
            }
        }
        let groups = finals.map(ordered).sorted { a, b in
            let wa = sizes[a[0]] * UInt64(a.count - 1), wb = sizes[b[0]] * UInt64(b.count - 1)
            return wa != wb ? wa > wb : paths[a[0]] < paths[b[0]]
        }
        return groups.map { $0.map { files[$0] } }
    }

    /// Splits every group into the files whose digests are equal; only subgroups of two or more survive.
    /// Files are read in tree order, not group by group, so a hard disk does not jump between far-apart groups.
    private static func refine(_ groups: [[Int]], paths: [String], sizes: [UInt64], edges: Bool, stage: Int, threads: Int,
                               cancel: Cancel, progress: Progress?) -> [[Int]] {
        var tasks: [(idx: Int, g: Int)] = []
        for (g, members) in groups.enumerated() { for i in members { tasks.append((i, g)) } }
        tasks.sort { $0.idx < $1.idx }
        progress?.begin(stage: stage, total: tasks.count)

        let lock = NSLock()
        var digests = [Data?](repeating: nil, count: tasks.count)
        var next = 0
        DispatchQueue.concurrentPerform(iterations: max(1, min(threads, 8))) { _ in
            var buf = [UInt8](repeating: 0, count: 1 << 20)
            while true {
                lock.lock()
                let t = next
                next += 1
                lock.unlock()
                if t >= tasks.count || cancel.isCancelled { return }
                let i = tasks[t].idx
                let d = digest(paths[i], size: sizes[i], edges: edges, buf: &buf, cancel: cancel)
                lock.lock()
                digests[t] = d
                lock.unlock()
                progress?.tick()
            }
        }
        if cancel.isCancelled { return [] }

        var by = [[Data: [Int]]](repeating: [:], count: groups.count)
        for (t, task) in tasks.enumerated() { if let d = digests[t] { by[task.g][d, default: []].append(task.idx) } }
        return by.flatMap { $0.values.filter { $0.count > 1 } }
    }

    /// SHA-256 of a whole file, or of its first and last 64 KB (the whole file when it is at most 128 KB).
    /// nil when the file cannot be read or is not the size it had when it was listed.
    private static func digest(_ path: String, size: UInt64, edges: Bool, buf: inout [UInt8], cancel: Cancel) -> Data? {
        let fd = open(path, O_RDONLY)
        if fd < 0 { return nil }
        defer { close(fd) }
        var st = stat()
        guard fstat(fd, &st) == 0, UInt64(st.st_size) == size else { return nil }
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
        return Data(hasher.finalize())
    }
}
