import Foundation

public enum CacheRisk: String {
    case low = "Low"        // rebuilt silently
    case medium = "Medium"  // browser caches: pages load slower once, some sites sign you out of nothing but may re-download data
}

public struct CacheCategory: Identifiable, Hashable {
    public var id: String { path }
    public let name: String
    public let path: String
    public let risk: CacheRisk
    public let leave: [String]  // names directly inside `path` that belong to another category and are left alone

    public func hash(into h: inout Hasher) { h.combine(path) }
    public static func == (a: CacheCategory, b: CacheCategory) -> Bool { a.path == b.path }
}

public struct CacheMeasure {
    public var files = 0
    public var bytes: UInt64 = 0
    public var entries: [String] = []  // the items that would go to the Trash (direct children of the category folder)
}

public enum CacheCleaner {
    private static let browserCacheFolders: [(String, String)] = [  // (name, folder inside ~/Library/Caches)
        ("Google Chrome", "Google/Chrome"), ("Microsoft Edge", "Microsoft Edge"), ("Brave", "BraveSoftware/Brave-Browser"),
        ("Arc", "Arc"), ("Vivaldi", "Vivaldi"), ("Opera", "com.operasoftware.Opera"),
    ]
    private static let chromiumSupport: [(String, String)] = [  // (name, folder inside ~/Library/Application Support)
        ("Google Chrome", "Google/Chrome"), ("Microsoft Edge", "Microsoft Edge"), ("Brave", "BraveSoftware/Brave-Browser"),
        ("Arc", "Arc/User Data"), ("Vivaldi", "Vivaldi"),
    ]

    private static func exists(_ p: String) -> Bool {
        var isDir: ObjCBool = false
        return FileManager.default.fileExists(atPath: p, isDirectory: &isDir) && isDir.boolValue
    }

    private static func subfolders(_ p: String) -> [String] {
        ((try? FileManager.default.contentsOfDirectory(atPath: p)) ?? []).filter { exists(p + "/" + $0) }
    }

    /// Cache locations that exist on this Mac. Every one is inside the user's own Library (or a tool folder in the home folder).
    public static func discover(home: String = NSHomeDirectory()) -> [CacheCategory] {
        let lib = home + "/Library"
        var out: [CacheCategory] = []
        var leave: [String] = ["Google", "Microsoft Edge", "BraveSoftware", "Firefox", "com.apple.Safari", "Arc", "Vivaldi", "com.operasoftware.Opera"]

        func add(_ name: String, _ path: String, _ risk: CacheRisk, leave l: [String] = []) {
            if exists(path) { out.append(CacheCategory(name: name, path: path, risk: risk, leave: l)) }
        }

        add("Application caches", lib + "/Caches", .low, leave: leave)
        leave.removeAll()
        add("Application logs", lib + "/Logs", .low)
        add("Xcode build data (DerivedData)", lib + "/Developer/Xcode/DerivedData", .low)
        add("npm cache", home + "/.npm/_cacache", .low)

        for (name, rel) in browserCacheFolders { add(name + " cache", lib + "/Caches/" + rel, .medium) }
        add("Safari cache", lib + "/Caches/com.apple.Safari", .medium)
        for p in subfolders(lib + "/Caches/Firefox/Profiles") { add("Firefox cache (\(p))", lib + "/Caches/Firefox/Profiles/" + p + "/cache2", .medium) }

        for (name, rel) in chromiumSupport {
            let base = lib + "/Application Support/" + rel
            for prof in subfolders(base) where prof == "Default" || prof.hasPrefix("Profile ") {
                for kind in ["Code Cache", "GPUCache", "Cache"] { add("\(name) \(kind) (\(prof))", base + "/" + prof + "/" + kind, .medium) }
            }
        }
        return out
    }

    /// An item may be cleaned only when it sits inside one of the discovered cache folders and is not a link.
    public static func isValid(_ path: String, home: String = NSHomeDirectory()) -> Bool {
        var st = stat()
        if lstat(path, &st) != 0 || (st.st_mode & 0o170000) == 0o120000 { return false }
        return discover(home: home).contains { path.hasPrefix($0.path + "/") && !path.contains("/../") }
    }

    /// Looks inside a category. `olderThan` = 0 means everything; otherwise only items not modified for that many seconds.
    public static func measure(_ cat: CacheCategory, olderThan seconds: Double) -> CacheMeasure {
        var m = CacheMeasure()
        let cutoff = Date().timeIntervalSince1970 - seconds
        let names = ((try? FileManager.default.contentsOfDirectory(atPath: cat.path)) ?? []).filter { !cat.leave.contains($0) }
        for n in names {
            let p = cat.path + "/" + n
            let w = DiskUtil.walk(p)
            if seconds > 0 && w.newest >= cutoff { continue }
            if w.bytes == 0 && w.files == 0 { continue }
            m.files += w.files
            m.bytes += w.bytes
            m.entries.append(p)
        }
        return m
    }
}
