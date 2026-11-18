import Foundation

public enum Protection: Int, Comparable {
    case normal = 0   // can be moved to the Trash
    case caution      // allowed, but apps or tools may depend on it (app data, dot-folders, libraries)
    case blocked      // never offered for deletion

    public static func < (a: Protection, b: Protection) -> Bool { a.rawValue < b.rawValue }
}

/// What this app will and will not touch. Everything the app deletes goes through `classify`.
public struct ProtectionRules {
    public let home: String

    public init(home: String = NSHomeDirectory()) {
        var h = home
        while h.count > 1 && h.hasSuffix("/") { h.removeLast() }
        self.home = h
    }

    /// The operating system and installed software. Never listed in Large/Old Files or Search, never deleted.
    static let systemPrefixes = ["/System", "/Library", "/Applications", "/usr", "/bin", "/sbin", "/private", "/etc", "/var", "/opt", "/cores", "/dev"]

    /// Folders of the user's profile that must stay (their contents may be removed, the folder itself may not).
    static let keepFolders: Set<String> = ["Library", "Desktop", "Documents", "Downloads", "Movies", "Music", "Pictures", "Public", "Applications"]

    private func under(_ path: String, _ prefix: String) -> Bool { path == prefix || path.hasPrefix(prefix + "/") }

    static func normalize(_ p: String) -> String {
        var s = p
        while s.count > 1 && s.hasSuffix("/") { s.removeLast() }
        return s
    }

    public func isSystemLocation(_ raw: String) -> Bool {
        let p = Self.normalize(raw)
        return Self.systemPrefixes.contains { under(p, $0) }
    }

    public func classify(_ raw: String) -> Protection {
        let p = Self.normalize(raw)
        if p == "/" || p == "/Volumes" || p == "/Users" { return .blocked }
        if isSystemLocation(p) { return .blocked }
        if p == home { return .blocked }
        if p.hasPrefix("/Users/") && p.split(separator: "/").count == 2 { return .blocked }  // somebody's home folder
        if under(p, home) {
            let rel = String(p.dropFirst(home.count + 1))
            let comps = rel.split(separator: "/")
            guard let firstSub = comps.first else { return .blocked }
            let first = String(firstSub)
            if comps.count == 1 && Self.keepFolders.contains(first) { return .blocked }
            if first == ".ssh" || first == ".gnupg" { return .blocked }  // keys: never
            if first == "Library" || first.hasPrefix(".") { return .caution }
        }
        let lower = p.lowercased()
        if lower.hasSuffix(".app") || lower.hasSuffix(".photoslibrary") || lower.hasSuffix(".musiclibrary") || lower.hasSuffix(".tvlibrary") { return .caution }
        return .normal
    }

    /// "/Users/<name>/Library" and the dot-folders tools keep in a profile (.config, .npm, .docker ...): managed by those tools.
    public func isAppData(_ path: String) -> Bool {
        guard path.hasPrefix("/Users/") else { return false }
        let comps = path.split(separator: "/")
        guard comps.count == 3 else { return false }
        return comps[2] == "Library" || (comps[2].hasPrefix(".") && comps[2].count > 1)
    }

    /// Lists skip a whole folder when it is system software, or app data while "Show app data" is off.
    /// Only the first levels of a scan are looked at (everything below inherits the answer), so this stays cheap.
    public func skipSubtree(_ n: Node, showAppData: Bool) -> Bool {
        guard n.isDir else { return false }
        var d = 0
        var p = n.parent
        while let q = p, d < 5 {
            d += 1
            p = q.parent
        }
        if d >= 5 { return false }
        let path = n.path
        return isSystemLocation(path) || (!showAppData && isAppData(path))
    }
}
