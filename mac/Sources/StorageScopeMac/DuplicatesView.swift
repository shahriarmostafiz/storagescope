import StorageScopeCore
import SwiftUI

/// One file of a group of duplicates. Paths, not nodes: a node of an old scan must never be touched once the scan is replaced.
private struct DupItem: Identifiable, Hashable {
    let path: String
    let size: UInt64
    let modified: Double
    var id: String { path }
    var name: String { (path as NSString).lastPathComponent }
    var location: String { (path as NSString).deletingLastPathComponent }
}

private struct DupRow: Identifiable {
    let item: DupItem
    let group: Int
    let original: String?  // nil = this is the original (kept); otherwise the path of the file this is a copy of
    var id: String { item.path }
}

/// Files with exactly the same content, grouped. The first file of each group (the oldest) is the original and is kept; the others are
/// selected for removal and say which file they are a copy of. An original can never be selected, so one copy always stays.
struct DuplicatesView: View {
    @EnvironmentObject var model: AppModel
    @AppStorage("dupMinKB") private var minKB = 1000
    @State private var typeIdx = 0  // 0 = all types
    @State private var groups: [[DupItem]] = []
    @State private var rows: [DupRow] = []
    @State private var selection = Set<String>()
    @State private var running = false
    @State private var progressText = ""
    @State private var emptyText = "Press Find Duplicates to look for files with identical content in your scan."
    @State private var cancel = DuplicateFinder.Cancel()

    private var originals: Set<String> { Set(groups.compactMap { $0.first?.path }) }
    private var copies: [DupItem] { groups.flatMap { $0.dropFirst() } }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 14) {
                Picker("Bigger than", selection: $minKB) {
                    Text("100 KB").tag(100)
                    Text("1 MB").tag(1000)
                    Text("10 MB").tag(10_000)
                    Text("100 MB").tag(100_000)
                }.frame(width: 190).disabled(running)
                Picker("Type", selection: $typeIdx) {
                    Text("All types").tag(0)
                    ForEach(FileCategory.allCases, id: \.rawValue) { Text($0.title).tag($0.rawValue + 1) }
                }.frame(width: 230).disabled(running)
                Button(running ? "Stop" : "Find Duplicates") { running ? cancel.cancel() : start() }
                    .buttonStyle(.borderedProminent)
                    .disabled(!running && (model.root == nil || model.busy))
                Spacer()
                Image(systemName: "info.circle")
                    .help("Two files are duplicates only when their contents are exactly the same; names and dates do not matter. In each group the oldest file is the original and is kept; pick another with Keep This One. Only files already in the scan are checked. System software and the insides of .git, .svn and .hg folders are never listed. A hard link looks like a copy, but removing it frees no space.")
            }
            .padding(12)

            HStack {
                Text(running ? progressText : summary).font(.callout).foregroundStyle(.secondary).lineLimit(1)
                Spacer()
                let all = !copies.isEmpty && selection.count == copies.count
                Button(all ? "Clear selection" : "Select all duplicates") {
                    selection = all ? [] : Set(copies.map { $0.path })
                }
                .disabled(copies.isEmpty || running)
            }
            .padding(.horizontal, 12).padding(.vertical, 8)

            if rows.isEmpty {
                Text(running ? "" : emptyText).foregroundStyle(.secondary).multilineTextAlignment(.center).padding(40)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                Table(rows, selection: $selection) {
                    TableColumn("Name") { r in
                        HStack(spacing: 6) {
                            Image(systemName: r.original == nil ? "checkmark.seal" : "doc.on.doc")
                                .foregroundStyle(r.original == nil ? Color.green : .secondary)
                            Text(r.item.name).lineLimit(1)
                        }
                    }
                    TableColumn("Size") { r in Text(Format.bytes(r.item.size)).monospacedDigit() }
                        .width(min: 70, ideal: 90, max: 120)
                    TableColumn("Group") { r in Text("#\(r.group + 1)") }
                        .width(min: 50, ideal: 60, max: 80)
                    TableColumn("Modified") { r in Text(Format.date(r.item.modified)).foregroundStyle(.secondary) }
                        .width(min: 110, ideal: 140, max: 180)
                    TableColumn("Location") { r in Text(r.item.location).lineLimit(1).truncationMode(.head).foregroundStyle(.secondary) }
                    TableColumn("Duplicate of") { r in
                        Text(r.original.map { "Copy of " + $0 } ?? "Original - kept").lineLimit(1).truncationMode(.head)
                            .foregroundStyle(r.original == nil ? Color.green : .secondary)
                    }
                }
                .contextMenu(forSelectionType: String.self) { ids in
                    if ids.count == 1, let p = ids.first {
                        Button("Show in Finder") { reveal(p) }
                        if !originals.contains(p) { Button("Keep This One") { keep(p) } }
                    }
                    Button("Move to Trash...") { trash(ids) }.disabled(copiesIn(ids).isEmpty)
                }
            }

            Divider()
            HStack {
                let sel = copiesIn(selection)
                Text(sel.isEmpty ? "Select copies to remove them" : "\(sel.count) selected, \(Format.bytes(sel.reduce(UInt64(0)) { $0 + $1.size }))")
                    .font(.callout).foregroundStyle(.secondary)
                Spacer()
                Button("Show in Finder") { if selection.count == 1, let p = selection.first { reveal(p) } }.disabled(selection.count != 1)
                Button("Keep This One") { if let p = selection.first { keep(p) } }
                    .disabled(selection.count != 1 || originals.contains(selection.first ?? ""))
                Button("Move to Trash...") { trash(selection) }
                    .disabled(sel.isEmpty || running || model.busy)
                    .keyboardShortcut(.delete, modifiers: .command)
            }
            .padding(10)
        }
        .onChange(of: selection) { _ in
            let ok = selection.subtracting(originals)  // an original is never selectable
            if ok != selection { selection = ok }
        }
        .onChange(of: model.revision) { _ in prune() }
    }

    private var summary: String {
        if groups.isEmpty { return "" }
        let n = copies.count
        let freed = groups.reduce(UInt64(0)) { $0 + ($1.first?.size ?? 0) * UInt64($1.count - 1) }
        return "\(groups.count) group\(groups.count == 1 ? "" : "s"), \(n) duplicate file\(n == 1 ? "" : "s"), \(Format.bytes(freed)) can be freed"
    }

    private func copiesIn(_ ids: Set<String>) -> [DupItem] { copies.filter { ids.contains($0.path) } }

    private func rebuild() {
        rows = groups.enumerated().flatMap { g, items in
            items.enumerated().map { i, it in DupRow(item: it, group: g, original: i == 0 ? nil : items[0].path) }
        }
    }

    // MARK: finding

    private func start() {
        guard let root = model.root, !running else { return }
        running = true
        groups = []
        rows = []
        selection = []
        progressText = "Sorting files by size..."
        let flag = DuplicateFinder.Cancel()
        cancel = flag
        let progress = DuplicateFinder.Progress()
        let skip = model.skip(model.showAppData)
        let minSize = UInt64(minKB) * 1000
        let cat: FileCategory? = typeIdx > 0 ? FileCategory(rawValue: typeIdx - 1) : nil
        Task { @MainActor in
            let poll = Task { @MainActor in  // show the progress while the search runs
                while !Task.isCancelled {
                    let s = progress.snapshot
                    let name = s.stage == 1 ? "Step 1 of 2: quick check, " : s.stage == 2 ? "Step 2 of 2: comparing contents, " : "Looking at "
                    progressText = name + "\(s.done) of \(s.total) files"
                    try? await Task.sleep(nanoseconds: 300_000_000)
                }
            }
            let (found, checked): ([[DupItem]], Int) = await Task.detached(priority: .userInitiated) {
                let files = DuplicateFinder.candidates(root: root, minSize: minSize, category: cat, skip: skip)
                let g = DuplicateFinder.find(files, cancel: flag, progress: progress)
                return (g.map { $0.map { DupItem(path: $0.path, size: $0.size, modified: $0.modified) } }, files.count)
            }.value
            poll.cancel()
            running = false
            if flag.isCancelled {
                emptyText = "Stopped. Press Find Duplicates to start again."
                return
            }
            groups = found
            selection = Set(found.flatMap { $0.dropFirst().map { $0.path } })  // every copy is selected, the original is not
            emptyText = "No duplicates found among \(checked) file\(checked == 1 ? "" : "s") of \(Format.bytes(minSize)) or more."
            rebuild()
        }
    }

    // MARK: actions

    private func node(_ path: String) -> Node? {
        guard let n = model.root?.find(path: path), !n.removed else { return nil }
        return n
    }

    private func reveal(_ path: String) { if let n = node(path) { model.showInFinder(n) } }

    private func keep(_ path: String) {  // make this copy the original of its group
        guard let g = groups.firstIndex(where: { $0.contains { $0.path == path } }), let i = groups[g].firstIndex(where: { $0.path == path }), i > 0 else { return }
        let old = groups[g][0].path
        let moved = groups[g].remove(at: i)
        groups[g].insert(moved, at: 0)
        selection.remove(path)
        selection.insert(old)  // the file that used to be kept is now a copy
        rebuild()
    }

    private func trash(_ ids: Set<String>) {
        let sel = copiesIn(ids)
        guard !sel.isEmpty else { return }
        let chosen = Set(sel.map { $0.path })
        for g in groups where g.dropFirst().contains(where: { chosen.contains($0.path) }) {  // each group must still have its original on disk
            if !FileManager.default.fileExists(atPath: g[0].path) {
                model.alert("An original file is missing", "\"\(g[0].path)\" is no longer on the disk, so its copies are not removed. Press Find Duplicates again.")
                return
            }
        }
        let nodes = sel.compactMap { node($0.path) }
        if nodes.isEmpty { return prune() }
        model.confirmAndTrash(nodes)
    }

    /// Forget files that are gone (trashed here or elsewhere); a group with fewer than two files is no longer a group.
    private func prune() {
        if groups.isEmpty { return }
        guard model.root != nil else {
            groups = []
            rows = []
            selection = []
            return
        }
        if copies.count + groups.count > 20_000 { return }  // a lookup per file; beyond this the list is left alone until the next search
        groups = groups.map { $0.filter { node($0.path) != nil } }.filter { $0.count >= 2 }
        selection = selection.intersection(Set(copies.map { $0.path }))
        if groups.isEmpty { emptyText = "No duplicates are left. Press Find Duplicates to search again." }
        rebuild()
    }
}
