import AppKit
import StorageScopeCore
import SwiftUI

enum Page: String, CaseIterable, Identifiable {
    case overview = "Overview"
    case browse = "Storage Analyzer"
    case search = "Search"
    case treemap = "Treemap"
    case large = "Large Files"
    case old = "Old Files"
    case duplicates = "Duplicate Files"
    case cache = "Cache & Logs"

    var id: String { rawValue }

    var icon: String {
        switch self {
        case .overview: return "internaldrive"
        case .browse: return "list.bullet.indent"
        case .search: return "magnifyingglass"
        case .treemap: return "square.grid.3x3.fill"
        case .large: return "arrow.up.doc"
        case .old: return "clock.arrow.circlepath"
        case .duplicates: return "doc.on.doc"
        case .cache: return "sparkles"
        }
    }
}

/// Shared state: the current scan, its search index, and the one place where files are moved to the Trash.
@MainActor
final class AppModel: ObservableObject {
    @Published var selectedPage: Page? = .overview  // optional: the sidebar List binds to it
    var page: Page { selectedPage ?? .overview }

    /// What Storage Analyzer and Treemap show. While a scan runs it is the tree being scanned (it fills in; see `live`).
    @Published var root: Node?
    @Published var index: SearchIndex?
    @Published var scanning = false
    @Published var live = false      // `root` is still being filled by a running scan: pages that need the finished scan wait
    @Published var loading = false   // the saved scan is being read
    @Published var saving = false    // the scan is being saved (a few seconds after a scan)
    @Published var progress = ""     // the folder being scanned (never a file)
    @Published var percent: Int? = nil  // nil = unknown (a folder scan has no known total)
    @Published var timeLeft = ""
    @Published var scanStats = ""
    @Published var showDetails = false
    @Published var unreadable = 0
    @Published var summary = ""
    @Published var revision = 0      // bumped whenever the tree changes (scan growing, finished, items trashed): lists recompute
    @Published var showAppData: Bool {
        didSet {
            UserDefaults.standard.set(showAppData, forKey: "showAppData")
            revision += 1
        }
    }
    @Published var rememberScan: Bool {
        didSet {
            UserDefaults.standard.set(rememberScan, forKey: "rememberScan")
            if !rememberScan { Snapshot.delete() }
        }
    }

    let rules = ProtectionRules()
    private var cancelFlag = CancelFlag()
    private var liveProgress = ScanProgress()
    private var timer: Timer?
    private var previousRoot: Node?     // restored when a scan is stopped
    private var previousIndex: SearchIndex?
    private var expectedBytes: UInt64 = 0
    private var scanScale = 0.9         // how much of a volume's used space a scan really finds as files (learned, saved)
    private var scanStart = Date()
    private var eta = EtaEstimator()
    private var expectedFiles = 0  // files the previous scan found in this folder (0 = no earlier scan to learn from)
    private var lastLiveBump = Date.distantPast
    private var savePending = false

    var busy: Bool { scanning || saving || loading }

    init() {
        showAppData = UserDefaults.standard.bool(forKey: "showAppData")
        rememberScan = UserDefaults.standard.object(forKey: "rememberScan") as? Bool ?? true
        let s = UserDefaults.standard.double(forKey: "scanScale")
        if s >= 0.3 && s <= 1.1 { scanScale = s }
        if rememberScan { loadSaved() }
    }

    // MARK: saved scan

    private func loadSaved() {
        loading = true
        Task.detached(priority: .userInitiated) { [weak self] in
            let l = Snapshot.load()
            let idx: SearchIndex? = l.map { SearchIndex(root: $0.root) }
            await MainActor.run { self?.finishLoad(l, idx) }
        }
    }

    private func finishLoad(_ l: Snapshot.Loaded?, _ idx: SearchIndex?) {
        loading = false
        guard let l, root == nil, !scanning else { return }
        root = l.root
        index = idx
        let f = DateFormatter()
        f.dateStyle = .medium
        f.timeStyle = .short
        summary = "Saved scan from \(f.string(from: l.scannedAt)). Press Scan to refresh it."
        revision += 1
    }

    private func startSave() {
        guard rememberScan, let r = root, !scanning else { return }
        if saving {
            savePending = true
            return
        }
        saving = true
        Task.detached(priority: .utility) { [weak self] in
            Snapshot.save(root: r)
            await MainActor.run { self?.finishSave() }
        }
    }

    private func finishSave() {
        saving = false
        if savePending {
            savePending = false
            startSave()
        }
    }

    func clearSavedScan() { Snapshot.delete() }

    // MARK: scanning

    func scanWholeDisk() { scan(path: "/") }

    func chooseFolderAndScan() {
        if let p = pickFolders(title: "Choose a folder to scan", multiple: false).first { scan(path: p) }
    }

    /// Used bytes of the volume(s) a whole-disk scan covers: the system volume plus the data volume. 0 = unknown (a folder scan).
    private func usedBytes(for path: String) -> UInt64 {
        func used(_ p: String) -> UInt64 {
            var s = statfs()
            guard statfs(p, &s) == 0, s.f_blocks >= s.f_bfree else { return 0 }
            return (s.f_blocks - s.f_bfree) * UInt64(s.f_bsize)
        }
        return path == "/" ? used("/") + used("/System/Volumes/Data") : 0
    }

    func scan(path: String) {
        guard !scanning && !saving && !loading else { return }
        scanning = true
        live = false
        previousRoot = root
        previousIndex = index
        progress = "Starting..."
        scanStats = ""
        percent = nil
        timeLeft = ""
        unreadable = 0
        expectedBytes = UInt64(Double(usedBytes(for: path)) * scanScale)
        scanStart = Date()
        expectedFiles = 0
        if let old = previousRoot, !old.isBuilding, let n = old.find(path: path), n.isDir, n.files >= 1000 { expectedFiles = n.files }
        eta = EtaEstimator(cautious: expectedFiles == 0)  // a first scan only has the used space to go on: be careful
        let prog = ScanProgress()
        let flag = CancelFlag()
        liveProgress = prog
        cancelFlag = flag
        timer = Timer.scheduledTimer(withTimeInterval: 0.25, repeats: true) { [weak self] _ in
            Task { @MainActor in self?.updateProgress() }
        }
        Task.detached(priority: .userInitiated) { [weak self] in
            let started = Date()
            let node = DiskScanner.scan(path: path, progress: prog, onRoot: { r in
                Task { @MainActor in self?.beginLive(r) }  // the pages may browse the tree while it fills
            }, isCancelled: { flag.isCancelled })
            let canceled = flag.isCancelled
            let idx: SearchIndex? = canceled ? nil : SearchIndex(root: node)
            let secs = Date().timeIntervalSince(started)
            await MainActor.run { self?.finishScan(node, canceled, idx, secs, prog.snapshot.unreadable) }
        }
    }

    func cancelScan() {
        cancelFlag.cancel()
        progress = "Stopping..."
    }

    private func beginLive(_ r: Node) {
        guard scanning else { return }
        root = r
        index = nil
        live = true
        revision += 1
    }

    private func updateProgress() {
        let s = liveProgress.snapshot
        let elapsed = Date().timeIntervalSince(scanStart)
        let byFiles = expectedFiles > 0
        if byFiles || expectedBytes > 0 {
            // Time depends on how many files there are, not on how big they are: a disk of many small files takes far longer than the same
            // amount of data in big files. So the file count of the previous scan is used when there is one; the used space is only the
            // rough guide of a first scan. If the folder grew past the old count the yardstick grows with it.
            let done = byFiles ? Double(s.files) : Double(s.bytes)
            let total = byFiles ? max(Double(expectedFiles), Double(s.files) * 1.1) : Double(expectedBytes)
            let e = eta.update(elapsed: elapsed, done: done, total: total)
            percent = Int(e.frac * 100)
            if e.frac >= 0.97 {
                timeLeft = "Almost done"
            } else if !e.ready {
                timeLeft = "Time left: estimating..."
            } else {
                timeLeft = "Time left: about " + Format.duration(e.seconds) + (byFiles ? "" : " (rough: first scan)")
            }
        } else {
            percent = nil
            timeLeft = ""
        }
        progress = s.current
        scanStats = "Files: \(Format.count(s.files))     Folders: \(Format.count(s.folders))     Size analyzed: \(Format.bytes(s.bytes))     Elapsed: \(Format.duration(elapsed))"
        if live && Date().timeIntervalSince(lastLiveBump) > 1 {  // the page being browsed catches up with the scan about once a second
            lastLiveBump = Date()
            revision += 1
        }
    }

    private func finishScan(_ node: Node, _ canceled: Bool, _ idx: SearchIndex?, _ seconds: Double, _ unread: Int) {
        timer?.invalidate()
        timer = nil
        scanning = false
        live = false
        percent = nil
        timeLeft = ""
        progress = ""
        if canceled {  // the half-scanned tree is dropped; what was there before stays
            root = previousRoot
            index = previousIndex
            previousRoot = nil
            previousIndex = nil
            summary = "Scan stopped."
            revision += 1
            return
        }
        previousRoot = nil
        previousIndex = nil
        root = node
        index = idx
        unreadable = unread
        summary = "\(Format.count(node.files)) files, \(Format.bytes(node.size)) in \(String(format: "%.1f", seconds)) s"
        if expectedBytes > 0 && node.size > 0 {  // learn how much of the "used space" is really files, for the next percentage
            let unscaled = Double(expectedBytes) / scanScale
            let ratio = Double(node.size) / unscaled
            scanScale = min(1.1, max(0.3, (scanScale + ratio) / 2))
            UserDefaults.standard.set(scanScale, forKey: "scanScale")
        }
        revision += 1
        startSave()
    }

    private func reindex() {
        guard let r = root else { return }
        Task.detached(priority: .utility) { [weak self] in
            let i = SearchIndex(root: r)
            await MainActor.run { self?.index = i }
        }
    }

    // MARK: Trash (the only way anything is removed)

    func skip(_ showAppData: Bool) -> (Node) -> Bool {
        let r = rules
        return { r.skipSubtree($0, showAppData: showAppData) }
    }

    func canTrash(_ nodes: [Node]) -> Bool {
        !busy && !nodes.isEmpty && !nodes.contains { rules.classify($0.path) == .blocked }
    }

    func showInFinder(_ node: Node) {
        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: node.path)])
    }

    /// Asks once, moves the items to the Trash, fixes the totals and reports what did not work.
    func confirmAndTrash(_ nodes: [Node]) {
        if busy {
            alert("Please wait", "A scan or a save is still running. Try again in a moment.")
            return
        }
        let top = Node.topLevel(nodes)
        guard !top.isEmpty else { return }
        let pairs = top.map { ($0, $0.path) }
        if pairs.contains(where: { rules.classify($0.1) == .blocked }) {
            alert("Protected items", "System software, your main folders and keys cannot be moved to the Trash from here.")
            return
        }
        let bytes = top.reduce(UInt64(0)) { $0 + $1.size }
        let caution = pairs.contains { rules.classify($0.1) == .caution }
        let a = NSAlert()
        a.messageText = top.count == 1 ? "Move \"\(top[0].name)\" to the Trash?" : "Move \(top.count) items to the Trash?"
        a.informativeText = Format.bytes(bytes) + ". You can put them back from the Trash."
            + (caution ? "\n\nSome of these are app data or tool folders. Apps may depend on them." : "")
        a.alertStyle = caution ? .warning : .informational
        a.addButton(withTitle: "Move to Trash")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }

        let res = Trasher.trash(paths: pairs.map { $0.1 }, rules: rules)
        let done = Set(res.trashed)
        for (n, p) in pairs where done.contains(p) { n.detach() }
        revision += 1
        reindex()
        if !done.isEmpty { startSave() }  // the saved scan should not list what is gone
        if !res.failed.isEmpty {
            let lines = res.failed.prefix(5).map { "\($0.path): \($0.reason)" }.joined(separator: "\n")
            alert("\(res.failed.count) item(s) could not be moved", lines)
        }
    }

    func alert(_ title: String, _ text: String) {
        let a = NSAlert()
        a.messageText = title
        a.informativeText = text
        a.addButton(withTitle: "OK")
        a.runModal()
    }
}

/// Folder picker used for scanning and for the Search scope / skip lists.
@MainActor
func pickFolders(title: String, multiple: Bool) -> [String] {
    let p = NSOpenPanel()
    p.title = title
    p.canChooseFiles = false
    p.canChooseDirectories = true
    p.allowsMultipleSelection = multiple
    p.prompt = "Choose"
    return p.runModal() == .OK ? p.urls.map { $0.path } : []
}
