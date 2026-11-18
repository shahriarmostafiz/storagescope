import Foundation

/// The last scan, saved so the next launch starts with data. Binary, compressed (LZFSE), written atomically, and read back as untrusted
/// input (every length is bounded; a damaged or foreign file simply returns nil).
public enum Snapshot {
    private static let magic: UInt64 = 0x5353_4D31  // "SSM1"
    private static let maxNodes = 60_000_000
    private static let maxName = 65_535
    private static let maxDepth = 100_000

    public static var defaultURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("StorageScope", isDirectory: true)
        try? FileManager.default.createDirectory(at: base, withIntermediateDirectories: true)
        return base.appendingPathComponent("lastscan.bin")
    }

    public static func delete() { try? FileManager.default.removeItem(at: defaultURL) }

    public struct Loaded {
        public let root: Node
        public let scannedAt: Date
    }

    private struct Writer {
        var bytes: [UInt8] = []
        mutating func byte(_ v: UInt8) { bytes.append(v) }
        mutating func varint(_ v: UInt64) {
            var x = v
            while x >= 0x80 {
                bytes.append(UInt8(x & 0x7F) | 0x80)
                x >>= 7
            }
            bytes.append(UInt8(x))
        }
        mutating func string(_ s: String) {
            let u = Array(s.utf8)
            varint(UInt64(u.count))
            bytes.append(contentsOf: u)
        }
    }

    private struct Reader {
        let bytes: [UInt8]
        var pos = 0
        mutating func byte() -> UInt8? {
            guard pos < bytes.count else { return nil }
            let b = bytes[pos]
            pos += 1
            return b
        }
        mutating func varint() -> UInt64? {
            var shift: UInt64 = 0
            var result: UInt64 = 0
            while true {
                guard let b = byte(), shift <= 63 else { return nil }
                result |= UInt64(b & 0x7F) << shift
                if b & 0x80 == 0 { return result }
                shift += 7
            }
        }
        mutating func string() -> String? {
            guard let n = varint(), n <= UInt64(Snapshot.maxName), pos + Int(n) <= bytes.count else { return nil }
            let s = String(decoding: bytes[pos..<pos + Int(n)], as: UTF8.self)
            pos += Int(n)
            return s
        }
    }

    /// Writes `root` (a finished scan). Items moved to the Trash since the scan are left out.
    @discardableResult
    public static func save(root: Node, scannedAt: Date = Date(), to url: URL = defaultURL) -> Bool {
        var w = Writer()
        w.bytes.reserveCapacity(1 << 20)
        w.varint(magic)
        w.varint(UInt64(max(0, scannedAt.timeIntervalSince1970)))
        var stack: [Node] = [root]
        while let n = stack.popLast() {
            let live = n.isDir ? n.children.filter { !$0.removed } : []
            w.byte((n.isDir ? 1 : 0) | (n.unreadable ? 2 : 0))
            w.string(n.name)
            w.varint(n.size)
            w.varint(UInt64(max(0, n.files)))
            w.varint(UInt64(max(0, n.modified)))
            w.varint(UInt64(live.count))
            stack.append(contentsOf: live.reversed())  // pre-order: the first child is read next
        }
        do {
            let packed = try (Data(w.bytes) as NSData).compressed(using: .lzfse) as Data
            try packed.write(to: url, options: .atomic)
            return true
        } catch {
            return false
        }
    }

    public static func load(from url: URL = defaultURL) -> Loaded? {
        guard let packed = try? Data(contentsOf: url), let raw = try? (packed as NSData).decompressed(using: .lzfse) as Data else { return nil }
        var r = Reader(bytes: [UInt8](raw))
        guard r.varint() == magic, let stamp = r.varint() else { return nil }
        var root: Node? = nil
        var stack: [(node: Node, remaining: Int)] = []
        var total = 0
        while true {
            while let top = stack.last, top.remaining == 0 { stack.removeLast() }
            if root != nil && stack.isEmpty { break }  // everything was read
            guard let flags = r.byte(), let name = r.string(), let size = r.varint(), let files = r.varint(), let mod = r.varint(),
                  let childCount = r.varint() else { return nil }
            total += 1
            let isDir = flags & 1 != 0
            if total > maxNodes || childCount > UInt64(maxNodes) || (!isDir && childCount != 0) { return nil }
            let n = Node(name: name, isDir: isDir, size: size, files: Int(truncatingIfNeeded: files), modified: Double(mod))
            n.unreadable = flags & 2 != 0
            if let top = stack.last {
                n.parent = top.node
                top.node.children.append(n)
                stack[stack.count - 1].remaining -= 1
            } else {
                if root != nil { return nil }
                root = n
            }
            if isDir && childCount > 0 {
                if stack.count >= maxDepth { return nil }
                stack.append((n, Int(childCount)))
            }
        }
        guard let rootNode = root else { return nil }
        return Loaded(root: rootNode, scannedAt: Date(timeIntervalSince1970: Double(stamp)))
    }
}
