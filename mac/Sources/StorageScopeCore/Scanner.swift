import Darwin
import Foundation

/// Live numbers while a scan runs. Written by the scan threads, read by the UI timer.
public final class ScanProgress: @unchecked Sendable {
    private let lock = NSLock()
    private var files = 0
    private var folders = 0
    private var bytes: UInt64 = 0
    private var unreadable = 0
    private var current = ""

    public init() {}

    func add(files f: Int, bytes b: UInt64, current c: String) {
        lock.lock()
        files += f
        folders += 1
        bytes += b
        current = c
        lock.unlock()
    }

    func addUnreadable() {
        lock.lock()
        unreadable += 1
        lock.unlock()
    }

    public var snapshot: (files: Int, folders: Int, bytes: UInt64, unreadable: Int, current: String) {
        lock.lock()
        defer { lock.unlock() }
        return (files, folders, bytes, unreadable, current)
    }
}

/// Thread-safe "stop" switch for a running scan.
public final class CancelFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var flag = false
    public init() {}
    public func cancel() {
        lock.lock()
        flag = true
        lock.unlock()
    }
    public var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return flag
    }
}

public enum DiskScanner {
    /// Places that are not part of "this disk's files": other volumes, firmlink duplicates, devices, swap.
    /// Applied by exact path, so scanning a folder inside one of them still works.
    public static let defaultSkips: Set<String> = [
        "/Volumes", "/System/Volumes", "/dev", "/private/var/vm", "/.vol", "/cores", "/Network", "/.Spotlight-V100", "/.fseventsd",
    ]

    /// Scans `path` (a folder, or "/" for the whole disk) with the readdir/lstat calls directly: much faster than URL resource keys.
    /// Symbolic links are not followed and not counted. Sizes are allocated bytes (what the disk really uses).
    /// `onRoot` is called once, before any folder is listed, with the (still empty) root: a UI may start browsing it right away through
    /// `Node.kids` / `liveSize` while the scan fills it.
    public static func scan(path rawPath: String, skip: Set<String> = defaultSkips, progress: ScanProgress = ScanProgress(),
                            onRoot: ((Node) -> Void)? = nil, isCancelled: @escaping () -> Bool = { false }) -> Node {
        var path = rawPath
        while path.count > 1 && path.hasSuffix("/") { path.removeLast() }
        var st = stat()
        let ok = lstat(path, &st) == 0
        let root = Node(name: path, isDir: true, modified: ok ? Double(st.st_mtimespec.tv_sec) : 0)
        if !ok {
            root.unreadable = true
            progress.addUnreadable()
            onRoot?(root)
            return root
        }
        root.building = true
        onRoot?(root)
        scanDir(root, path, depth: 0, skip: skip, progress: progress, isCancelled: isCancelled)
        return root
    }

    private static func scanDir(_ node: Node, _ path: String, depth: Int, skip: Set<String>, progress: ScanProgress, isCancelled: () -> Bool) {
        if isCancelled() { return }
        guard let dir = opendir(path) else {
            Node.lock.lock()
            node.unreadable = true
            node.building = false  // nothing to list: the (empty) folder is final
            Node.lock.unlock()
            progress.addUnreadable()
            return
        }
        var kids: [Node] = []
        var subdirs: [(Node, String)] = []
        var fileBytes: UInt64 = 0
        var fileCount = 0
        while let ent = readdir(dir) {
            var raw = ent.pointee.d_name
            let name = withUnsafePointer(to: &raw) {
                $0.withMemoryRebound(to: CChar.self, capacity: Int(MAXPATHLEN)) { String(cString: $0) }
            }
            if name == "." || name == ".." { continue }
            let childPath = path == "/" ? "/" + name : path + "/" + name
            if skip.contains(childPath) { continue }
            var st = stat()
            if lstat(childPath, &st) != 0 { continue }
            let kind = st.st_mode & 0o170000
            let mtime = Double(st.st_mtimespec.tv_sec)
            if kind == 0o040000 {  // folder
                let c = Node(name: name, isDir: true, modified: mtime, parent: node)
                c.building = true  // until its own listing is published
                kids.append(c)
                subdirs.append((c, childPath))
            } else if kind == 0o100000 {  // regular file (links, sockets and devices are left out)
                let bytes = UInt64(st.st_blocks) * 512
                let c = Node(name: name, isDir: false, size: bytes, files: 1, modified: mtime, parent: node)
                kids.append(c)
                fileBytes += bytes
                fileCount += 1
            }
        }
        closedir(dir)  // closed before going deeper: a deep tree must not hold hundreds of open handles
        progress.add(files: fileCount, bytes: fileBytes, current: path)

        // Publish the listing, and count what was found for this folder and every folder above it right away, so a UI that browses the
        // tree during the scan sees sizes grow. (The exact totals are written below once everything under this folder is done.)
        Node.lock.lock()
        node.children = kids
        node.building = false
        var up: Node? = node
        while let u = up {
            u.size += fileBytes
            u.files += fileCount
            up = u.parent
        }
        Node.lock.unlock()

        if depth < 3 && subdirs.count > 1 {
            // top levels are scanned in parallel; below that one thread per branch is enough
            DispatchQueue.concurrentPerform(iterations: subdirs.count) { i in
                scanDir(subdirs[i].0, subdirs[i].1, depth: depth + 1, skip: skip, progress: progress, isCancelled: isCancelled)
            }
        } else {
            for (c, p) in subdirs { scanDir(c, p, depth: depth + 1, skip: skip, progress: progress, isCancelled: isCancelled) }
        }

        var total = fileBytes
        var count = fileCount
        for (c, _) in subdirs {
            total += c.size
            count += c.files
        }
        Node.lock.lock()
        node.size = total
        node.files = count
        Node.lock.unlock()
    }
}

/// Small helper used for cache folders: size, file count and newest modification time of a folder tree.
public enum DiskUtil {
    public static func walk(_ path: String) -> (bytes: UInt64, files: Int, newest: Double) {
        var st = stat()
        if lstat(path, &st) != 0 { return (0, 0, 0) }
        let kind = st.st_mode & 0o170000
        let mtime = Double(st.st_mtimespec.tv_sec)
        if kind == 0o100000 { return (UInt64(st.st_blocks) * 512, 1, mtime) }
        if kind != 0o040000 { return (0, 0, mtime) }
        var bytes: UInt64 = 0
        var files = 0
        var newest = 0.0
        if let dir = opendir(path) {
            var names: [String] = []
            while let ent = readdir(dir) {
                var raw = ent.pointee.d_name
                let name = withUnsafePointer(to: &raw) {
                    $0.withMemoryRebound(to: CChar.self, capacity: Int(MAXPATHLEN)) { String(cString: $0) }
                }
                if name != "." && name != ".." { names.append(name) }
            }
            closedir(dir)
            for n in names {
                let r = walk(path + "/" + n)
                bytes += r.bytes
                files += r.files
                newest = max(newest, r.newest)
            }
        }
        if files == 0 { newest = max(newest, mtime) }  // an empty folder counts by its own time
        return (bytes, files, newest)
    }
}
