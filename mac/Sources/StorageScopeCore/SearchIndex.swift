import Darwin
import Foundation

public enum SearchKind: Int { case all = 0, filesOnly, foldersOnly }

public struct SearchSpec {
    public var text = ""
    public var matchCase = false
    public var wholeWord = false
    public var regex = false
    public var showHidden = false               // names starting with "." are left out unless this is on
    public var kind: SearchKind = .all
    public var include = ""                     // "pdf, docx, *.log": only these
    public var exclude = ""                     // same syntax: leave these out
    public var category: FileCategory? = nil
    public var minSize: UInt64 = 0
    public var maxSize: UInt64 = .max
    public var modifiedAfter: Double? = nil     // seconds since 1970
    public var modifiedBefore: Double? = nil
    public var scopes: [Node] = []              // search only inside these folders (empty = the whole scan)
    public var excluded: [Node] = []            // folders to skip
    public var hide: ((Node) -> Bool)? = nil    // extra folders to skip (system software, app data)
    public var timeLimit: TimeInterval = 3      // a heavy regex is stopped after this long
    public var maxResults = 50_000
    public init() {}
}

public struct SearchResult {
    public var hits: [Node] = []
    public var truncated = false
    public var timedOut = false
    public var error: String? = nil
}

/// Flat, pre-order copy of the scan with all names lowercased in one byte pool.
/// A folder's subtree is the index range [i, end[i]), so "search only in this folder" is a range scan, and skipping a folder is one jump.
public final class SearchIndex {
    private var nodes: [Node] = []
    private var end: [Int32] = []
    private var off: [Int32] = []
    private var pool: [UInt8] = []

    public var count: Int { nodes.count }

    public init(root: Node) {
        var stack: [(node: Node, next: Int, idx: Int)] = []
        func add(_ n: Node) -> Int {
            let i = nodes.count
            nodes.append(n)
            end.append(0)
            off.append(Int32(pool.count))
            pool.append(contentsOf: n.name.lowercased().utf8)
            return i
        }
        stack.append((root, 0, add(root)))
        while !stack.isEmpty {
            let top = stack.count - 1
            let f = stack[top]
            if f.next >= f.node.children.count {
                end[f.idx] = Int32(nodes.count)
                stack.removeLast()
                continue
            }
            stack[top].next += 1
            let c = f.node.children[f.next]
            if c.removed { continue }
            let ci = add(c)
            if c.isDir && !c.children.isEmpty { stack.append((c, 0, ci)) } else { end[ci] = Int32(ci + 1) }
        }
        off.append(Int32(pool.count))
    }

    // MARK: query parsing

    private final class Needle {
        let ptr: UnsafeMutablePointer<UInt8>
        let len: Int
        let bytes: [UInt8]
        let glob: Bool
        let path: Bool
        let text: String
        init(_ s: String, glob: Bool, path: Bool) {
            let b = Array(s.utf8)
            let p = UnsafeMutablePointer<UInt8>.allocate(capacity: max(1, b.count))
            for (i, x) in b.enumerated() { p[i] = x }
            bytes = b
            len = b.count
            ptr = p
            self.glob = glob
            self.path = path
            text = s
        }
        deinit { ptr.deallocate() }
    }

    private struct Pat {
        var bytes: [UInt8]
        var glob: Bool
    }

    private static func isGlob(_ s: String) -> Bool { s.contains("*") || s.contains("?") }

    private static func parseTerms(_ text: String, matchCase: Bool) -> [Needle] {
        var out: [Needle] = []
        var cur = ""
        var quoted = false
        var inQuote = false
        func flush() {
            if cur.isEmpty { return }
            var s = matchCase ? cur : cur.lowercased()
            let isPath = s.contains("/")
            if isPath { s = s.replacingOccurrences(of: "\\", with: "/") }
            out.append(Needle(s, glob: !quoted && !isPath && isGlob(s), path: isPath))
            cur = ""
            quoted = false
        }
        for ch in text {
            if ch == "\"" {
                inQuote.toggle()
                quoted = quoted || inQuote
            } else if !inQuote && (ch == " " || ch == "\t") {
                flush()
            } else {
                cur.append(ch)
            }
        }
        flush()
        return out
    }

    private static func parseList(_ text: String) -> [Pat] {
        var out: [Pat] = []
        var cur = ""
        func flush() {
            if cur.isEmpty { return }
            let g = isGlob(cur)
            if !g && cur.hasPrefix(".") { cur.removeFirst() }
            if !cur.isEmpty { out.append(Pat(bytes: Array(cur.lowercased().utf8), glob: g)) }
            cur = ""
        }
        for ch in text {
            if ch == "," || ch == ";" || ch == " " || ch == "\t" { flush() } else { cur.append(ch) }
        }
        flush()
        return out
    }

    // MARK: byte matching

    private static func glob(_ p: [UInt8], _ s: UnsafePointer<UInt8>, _ n: Int) -> Bool {
        var pi = 0, si = 0, star = -1, mark = 0
        while si < n {
            if pi < p.count && (p[pi] == 63 || p[pi] == s[si]) {  // ? or same byte
                pi += 1
                si += 1
            } else if pi < p.count && p[pi] == 42 {  // *
                star = pi
                pi += 1
                mark = si
            } else if star >= 0 {
                pi = star + 1
                mark += 1
                si = mark
            } else {
                return false
            }
        }
        while pi < p.count && p[pi] == 42 { pi += 1 }
        return pi == p.count
    }

    private static func isWordByte(_ b: UInt8) -> Bool {
        (b >= 48 && b <= 57) || (b >= 65 && b <= 90) || (b >= 97 && b <= 122) || b == 95 || b >= 0x80
    }

    private static func contains(_ h: UnsafePointer<UInt8>, _ hn: Int, _ t: Needle, whole: Bool) -> Bool {
        if t.len == 0 { return true }
        if t.len > hn { return false }
        var start = 0
        while start + t.len <= hn {
            guard let r = memmem(h + start, hn - start, t.ptr, t.len) else { return false }
            let at = h.distance(to: r.assumingMemoryBound(to: UInt8.self))
            if !whole { return true }
            let left = at == 0 || !isWordByte(h[at - 1])
            let right = at + t.len >= hn || !isWordByte(h[at + t.len])
            if left && right { return true }
            start = at + 1
        }
        return false
    }

    /// "name.ext" ends with "." + ext (or matches the wildcard).
    private static func listMatches(_ l: [Pat], _ s: UnsafePointer<UInt8>, _ n: Int) -> Bool {
        for p in l {
            if p.glob {
                if glob(p.bytes, s, n) { return true }
            } else if n > p.bytes.count && s[n - p.bytes.count - 1] == 46 {
                var same = true
                for i in 0..<p.bytes.count where s[n - p.bytes.count + i] != p.bytes[i] {
                    same = false
                    break
                }
                if same { return true }
            }
        }
        return false
    }

    // MARK: search

    /// A group that repeats and itself contains a repeat, like (a+)+ or (.*x)*, can take exponential time in a regular-expression engine on some
    /// names, and one match cannot be interrupted. Such patterns are refused up front. Bounded repeats like {4} or {1,3} are fine.
    public static func repeatsARepeat(_ pattern: String) -> Bool {
        let p = Array(pattern.unicodeScalars)
        func unbounded(_ i: Int) -> Bool {
            if p[i] == "*" || p[i] == "+" { return true }
            guard p[i] == "{" else { return false }
            var j = i + 1
            while j < p.count && p[j] != "}" { j += 1 }
            return j < p.count && j > i + 1 && p[j - 1] == ","
        }
        var hasRepeat: [Bool] = []  // one entry per open group: does it contain a repeat?
        var inClass = false
        var i = 0
        while i < p.count {
            let c = p[i]
            if c == "\\" {
                i += 2
                continue
            }
            if inClass {
                if c == "]" { inClass = false }
                i += 1
                continue
            }
            if c == "[" {
                inClass = true
            } else if c == "(" {
                hasRepeat.append(false)
            } else if c == ")" {
                let inner = hasRepeat.last ?? false
                if !hasRepeat.isEmpty { hasRepeat.removeLast() }
                if inner && i + 1 < p.count && unbounded(i + 1) { return true }
                if inner && !hasRepeat.isEmpty { hasRepeat[hasRepeat.count - 1] = true }
            } else if unbounded(i) && !hasRepeat.isEmpty {
                hasRepeat[hasRepeat.count - 1] = true
            }
            i += 1
        }
        return false
    }

    public func run(_ sp: SearchSpec) -> SearchResult {
        var res = SearchResult()
        if nodes.isEmpty { return res }
        if sp.regex && !sp.text.isEmpty && (sp.text.count > 300 || SearchIndex.repeatsARepeat(sp.text)) {
            res.error = "This pattern repeats a repeat, like (a+)+, which can take far too long. Simplify it."
            return res
        }

        var terms: [Needle] = []
        var regex: NSRegularExpression? = nil
        if sp.regex && !sp.text.isEmpty {
            do {
                regex = try NSRegularExpression(pattern: sp.text, options: sp.matchCase ? [] : [.caseInsensitive])
            } catch {
                res.error = "This is not a valid regular expression."
                return res
            }
        } else {
            terms = Self.parseTerms(sp.text, matchCase: sp.matchCase)
        }
        let inc = Self.parseList(sp.include)
        let exc = Self.parseList(sp.exclude)
        let typeFilter = sp.category != nil || !inc.isEmpty
        let excluded = Set(sp.excluded.map { $0.id })

        // ranges to scan: everything, or the slice of each chosen folder (nested choices are not searched twice)
        var ranges: [(Int, Int)] = []
        if sp.scopes.isEmpty {
            ranges = [(0, nodes.count)]
        } else {
            for s in sp.scopes {
                if let i = nodes.firstIndex(where: { $0 === s }) { ranges.append((i + 1, Int(end[i]))) }
            }
            ranges.sort { $0.0 < $1.0 || ($0.0 == $1.0 && $0.1 < $1.1) }
            var merged: [(Int, Int)] = []
            for r in ranges where merged.isEmpty || r.0 >= merged[merged.count - 1].1 { merged.append(r) }
            ranges = merged
        }

        let deadline = Date().addingTimeInterval(sp.timeLimit)
        var tick = 0
        let mask = regex != nil ? 0x3F : 0xFFF
        let matchCase = sp.matchCase

        pool.withUnsafeBufferPointer { poolBuf in
            guard let base = poolBuf.baseAddress ?? UnsafePointer<UInt8>(bitPattern: 1) else { return }
            for (lo, hi) in ranges {
                var i = lo
                while i < hi {
                    tick += 1
                    if tick & mask == 0 && Date() > deadline {
                        res.timedOut = true
                        return
                    }
                    let n = nodes[i]
                    let nameLen = Int(off[i + 1] - off[i])
                    let lower = base + Int(off[i])
                    if n.removed || (!sp.showHidden && nameLen > 0 && lower[0] == 46) {
                        i = Int(end[i])
                        continue
                    }
                    if n.isDir && (excluded.contains(n.id) || (sp.hide?(n) ?? false)) {
                        i = Int(end[i])
                        continue
                    }
                    let at = i
                    i += 1
                    if n.parent == nil { continue }  // the scan root itself is not a result
                    if (sp.kind == .filesOnly && n.isDir) || (sp.kind == .foldersOnly && !n.isDir) { continue }
                    if n.isDir && typeFilter { continue }
                    if n.size < sp.minSize || n.size > sp.maxSize { continue }
                    if let a = sp.modifiedAfter, n.modified < a { continue }
                    if let b = sp.modifiedBefore, n.modified > b { continue }
                    if let c = sp.category, FileCategory.of(name: n.name) != c { continue }
                    if !inc.isEmpty && !Self.listMatches(inc, lower, nameLen) { continue }
                    if !exc.isEmpty && Self.listMatches(exc, lower, nameLen) { continue }

                    var ok = true
                    if let re = regex {
                        let name = n.name
                        ok = re.firstMatch(in: name, options: [], range: NSRange(location: 0, length: name.utf16.count)) != nil
                    } else if !terms.isEmpty {
                        // original-case bytes are only needed for "Match case"
                        let caseBytes: [UInt8] = matchCase ? Array(n.name.utf8) : []
                        ok = caseBytes.withUnsafeBufferPointer { cb in
                            let hay: UnsafePointer<UInt8> = matchCase ? (cb.baseAddress ?? base) : lower
                            let hn = matchCase ? caseBytes.count : nameLen
                            for t in terms {
                                var hit: Bool
                                if t.path {
                                    let p = matchCase ? n.path : n.path.lowercased()
                                    hit = p.contains(t.text)
                                } else if t.glob {
                                    hit = Self.glob(t.bytes, hay, hn)
                                } else {
                                    hit = Self.contains(hay, hn, t, whole: sp.wholeWord)
                                }
                                if !hit { return false }
                            }
                            return true
                        }
                    }
                    if !ok { continue }
                    _ = at
                    res.hits.append(n)
                    if res.hits.count >= sp.maxResults {
                        res.truncated = true
                        return
                    }
                }
            }
        }
        return res
    }
}
