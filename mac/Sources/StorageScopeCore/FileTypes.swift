import Foundation

public enum FileCategory: Int, CaseIterable {
    case documents, images, video, audio, archives, code, apps, diskImages, other

    public var title: String {
        switch self {
        case .documents: return "Documents"
        case .images: return "Images"
        case .video: return "Video"
        case .audio: return "Audio"
        case .archives: return "Archives"
        case .code: return "Code"
        case .apps: return "Apps and installers"
        case .diskImages: return "Disk images"
        case .other: return "Other"
        }
    }

    private static let table: [String: FileCategory] = {
        var t: [String: FileCategory] = [:]
        func add(_ c: FileCategory, _ exts: String) { for e in exts.split(separator: " ") { t[String(e)] = c } }
        add(.documents, "pdf doc docx xls xlsx ppt pptx pages numbers key txt rtf md csv odt ods epub")
        add(.images, "jpg jpeg png gif heic heif tiff tif bmp webp svg raw cr2 nef arw dng psd ai")
        add(.video, "mp4 mov m4v avi mkv wmv webm flv mpg mpeg")
        add(.audio, "mp3 m4a aac wav flac aiff ogg wma")
        add(.archives, "zip rar 7z tar gz tgz bz2 xz")
        add(.code, "swift c h cpp hpp m mm js ts tsx jsx py rb go rs java kt cs php html css json xml yml yaml sh sql")
        add(.apps, "app pkg ipa")
        add(.diskImages, "dmg iso img sparseimage sparsebundle")
        return t
    }()

    public static func of(name: String) -> FileCategory {
        guard let dot = name.lastIndex(of: "."), dot != name.startIndex else { return .other }
        return table[name[name.index(after: dot)...].lowercased()] ?? .other
    }
}

public enum Format {
    public static func bytes(_ b: UInt64) -> String {
        ByteCountFormatter.string(fromByteCount: Int64(clamping: b), countStyle: .file)  // decimal units, like Finder
    }

    /// 75 s -> "1:15", 3,725 s -> "1:02:05"
    public static func duration(_ seconds: Double) -> String {
        let t = max(0, Int(seconds.rounded()))
        let h = t / 3600, m = (t % 3600) / 60, s = t % 60
        return h > 0 ? String(format: "%d:%02d:%02d", h, m, s) : String(format: "%d:%02d", m, s)
    }

    public static func count(_ n: Int) -> String {
        let f = NumberFormatter()
        f.numberStyle = .decimal
        return f.string(from: NSNumber(value: n)) ?? "\(n)"
    }

    public static func date(_ t: Double) -> String {
        if t <= 0 { return "" }
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        return f.string(from: Date(timeIntervalSince1970: t))
    }
}
