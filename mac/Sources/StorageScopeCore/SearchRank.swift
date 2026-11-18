import Foundation

/// "Best match first" for search results: a name equal to the query (the extension is ignored, so "report" matches "report.pdf"), then names
/// that start with it, then whole-word hits, then names that merely contain it, then everything else the filters let through (several
/// words, wildcards, path words). Ties: the shorter name first, then A to Z.
public enum SearchRank {
    public static func order(_ nodes: [Node], query: String) -> [Node] {
        let q = query.replacingOccurrences(of: "\"", with: "").lowercased().trimmingCharacters(in: .whitespaces)
        if q.isEmpty { return nodes }

        func isWord(_ c: Character) -> Bool { c.isLetter || c.isNumber || c == "_" }

        struct Key {
            let tier: Int
            let len: Int
            let name: String
            let node: Node
        }
        var keys: [Key] = []
        keys.reserveCapacity(nodes.count)
        for n in nodes {
            let name = n.name.lowercased()
            var stem = name
            if !n.isDir, let dot = name.lastIndex(of: "."), dot != name.startIndex { stem = String(name[..<dot]) }
            var tier = 4
            if name == q || stem == q {
                tier = 0
            } else if name.hasPrefix(q) {
                tier = 1
            } else if name.range(of: q) != nil {
                tier = 3
                var searchRange = name.startIndex..<name.endIndex
                while let r = name.range(of: q, options: [], range: searchRange) {
                    let left = r.lowerBound == name.startIndex || !isWord(name[name.index(before: r.lowerBound)])
                    let right = r.upperBound == name.endIndex || !isWord(name[r.upperBound])
                    if left && right {
                        tier = 2
                        break
                    }
                    searchRange = r.upperBound..<name.endIndex
                }
            }
            keys.append(Key(tier: tier, len: name.count, name: name, node: n))
        }
        keys.sort { a, b in
            if a.tier != b.tier { return a.tier < b.tier }
            if a.len != b.len { return a.len < b.len }
            return a.name < b.name
        }
        return keys.map { $0.node }
    }
}
