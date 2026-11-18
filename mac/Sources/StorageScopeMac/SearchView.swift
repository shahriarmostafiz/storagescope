import StorageScopeCore
import SwiftUI

private let dateOptions: [(label: String, days: Int, newer: Bool)] = [
    ("Any time", 0, true), ("Last 7 days", 7, true), ("Last 30 days", 30, true), ("Last year", 365, true),
    ("Older than 3 months", 91, false), ("Older than 6 months", 183, false), ("Older than 1 year", 365, false), ("Older than 2 years", 730, false),
]

/// Search the scan (no disk access) through the name index: words, "phrases", wildcards, regex, type / size / date filters,
/// and "search only in" / "skip" folder lists.
struct SearchView: View {
    @EnvironmentObject var model: AppModel
    @State private var query = ""
    @State private var whereIdx = 0      // 0 = the whole scan, 1 = selected folders
    @State private var kindIdx = 0       // files and folders / files only / folders only
    @State private var typeIdx = 0
    @State private var include = ""
    @State private var exclude = ""
    @State private var minMB = ""
    @State private var maxMB = ""
    @State private var dateIdx = 0
    @State private var matchCase = false
    @State private var wholeWord = false
    @State private var useRegex = false
    @State private var showFilters = false
    @State private var scope: [String] = []
    @State private var skip: [String] = []
    @State private var rows: [Node] = []
    @State private var token = 0
    @State private var info = ""
    @State private var rankQuery = ""  // the query the rows answer: best match first (not for regex)
    @State private var emptyText = "Type something to search for, or open Filters to search by type, size or date."

    /// Everything that changes the result: when it changes the search runs again after a short pause.
    private var signature: String {
        [query, "\(whereIdx)", "\(kindIdx)", "\(typeIdx)", include, exclude, minMB, maxMB, "\(dateIdx)", "\(matchCase)", "\(wholeWord)", "\(useRegex)",
         scope.joined(separator: "|"), skip.joined(separator: "|"), "\(model.revision)", "\(model.showAppData)", "\(model.index?.count ?? 0)"].joined(separator: "\u{1}")
    }

    var body: some View {
        VStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 10) {
                HStack(spacing: 12) {
                    Picker("Search in", selection: $whereIdx) {
                        Text("Whole scan").tag(0)
                        Text("Selected folders").tag(1)
                    }.frame(width: 250)
                    Picker("Show", selection: $kindIdx) {
                        Text("Files and folders").tag(0)
                        Text("Files only").tag(1)
                        Text("Folders only").tag(2)
                    }.frame(width: 250)
                    Picker("Type", selection: $typeIdx) {  // always visible: the file type is one of the first things people search by
                        Text("All types").tag(0)
                        ForEach(FileCategory.allCases, id: \.rawValue) { Text($0.title).tag($0.rawValue + 1) }
                    }.frame(width: 220)
                    Spacer()
                }
                HStack {
                    TextField("Search files and folders: words, \"exact phrase\", *.pdf, report?.docx", text: $query)
                        .textFieldStyle(.roundedBorder)
                    Button(showFilters ? "Hide filters" : "Filters") { showFilters.toggle() }
                }
                if whereIdx == 1 {
                    FolderList(title: "Search only in", folders: $scope, pickTitle: "Choose folders to search in")
                }
                if showFilters { filters }
            }
            .padding(12)
            Divider()
            ResultsPanel(rows: rows, token: token, info: info, emptyText: emptyText, rankQuery: rankQuery)
        }
        .task(id: signature) {
            try? await Task.sleep(nanoseconds: 200_000_000)  // wait for a pause in typing
            if Task.isCancelled { return }
            await run()
        }
    }

    private var filters: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 12) {
                TextField("Only extensions: pdf, docx, *.log", text: $include).textFieldStyle(.roundedBorder)
                TextField("Leave out: tmp, *.bak", text: $exclude).textFieldStyle(.roundedBorder)
            }
            HStack(spacing: 12) {
                TextField("Min size (MB)", text: $minMB).textFieldStyle(.roundedBorder).frame(width: 130)
                TextField("Max size (MB)", text: $maxMB).textFieldStyle(.roundedBorder).frame(width: 130)
                Picker("Modified", selection: $dateIdx) {
                    ForEach(0..<dateOptions.count, id: \.self) { Text(dateOptions[$0].label).tag($0) }
                }.frame(width: 280)
                Toggle("Match case", isOn: $matchCase)
                Toggle("Whole word", isOn: $wholeWord)
                Toggle("Regex", isOn: $useRegex)
                Spacer()
            }
            FolderList(title: "Skip folders", folders: $skip, pickTitle: "Choose folders to skip")
        }
    }

    @MainActor private func run() async {
        guard let root = model.root, let idx = model.index else {
            rows = []
            token += 1
            info = ""
            emptyText = "Scan first, then search."
            return
        }
        var spec = SearchSpec()
        spec.text = query.trimmingCharacters(in: .whitespaces)
        spec.matchCase = matchCase
        spec.wholeWord = wholeWord
        spec.regex = useRegex
        spec.kind = SearchKind(rawValue: kindIdx) ?? .all
        spec.category = typeIdx > 0 ? FileCategory(rawValue: typeIdx - 1) : nil
        spec.include = include
        spec.exclude = exclude
        spec.minSize = UInt64((Double(minMB.trimmingCharacters(in: .whitespaces)) ?? 0) * 1_000_000)
        if let m = Double(maxMB.trimmingCharacters(in: .whitespaces)), m > 0 { spec.maxSize = UInt64(m * 1_000_000) }
        if dateIdx > 0 {
            let o = dateOptions[dateIdx]
            let cut = Date().addingTimeInterval(-Double(o.days) * 86_400).timeIntervalSince1970
            if o.newer { spec.modifiedAfter = cut } else { spec.modifiedBefore = cut }
        }
        spec.hide = model.skip(model.showAppData)

        var missing = 0
        if whereIdx == 1 {
            for p in scope {
                if let n = root.find(path: p) { spec.scopes.append(n) } else { missing += 1 }
            }
        }
        spec.excluded = skip.compactMap { root.find(path: $0) }

        let hasCriteria = !spec.text.isEmpty || spec.category != nil || !include.isEmpty || spec.minSize > 0 || spec.maxSize != .max || dateIdx > 0
        if whereIdx == 1 && scope.isEmpty {
            emptyText = "Add one or more folders above to search in."
            rows = []
        } else if whereIdx == 1 && spec.scopes.isEmpty {
            emptyText = "The chosen folders are not part of the current scan. Scan them first."
            rows = []
        } else if !hasCriteria {
            emptyText = "Type something to search for, or open Filters to search by type, size or date."
            rows = []
        } else {
            let result = await Task.detached(priority: .userInitiated) { idx.run(spec) }.value
            if Task.isCancelled { return }
            if let e = result.error {
                emptyText = e
                rows = []
            } else {
                rows = result.hits
                rankQuery = useRegex ? "" : spec.text
                var text = "\(rows.count) result\(rows.count == 1 ? "" : "s")"
                if result.truncated { text += "  -  showing the first 50,000, refine the search" }
                if result.timedOut { text += "  -  stopped after 3 seconds, refine the search" }
                if missing > 0 { text += "  -  \(missing) chosen folder\(missing == 1 ? " is" : "s are") not in the scan" }
                info = text
                emptyText = "Nothing matched. Check the filters, or try fewer words."
            }
        }
        if rows.isEmpty { info = "" }
        token += 1
    }
}

/// A small list of folders with Add / Remove (used for "Search only in" and "Skip folders").
private struct FolderList: View {
    let title: String
    @Binding var folders: [String]
    let pickTitle: String
    @State private var selected = Set<String>()

    var body: some View {
        HStack(alignment: .top, spacing: 10) {
            Text(title + ":").frame(width: 110, alignment: .leading)
            List(folders, id: \.self, selection: $selected) { Text($0).lineLimit(1).truncationMode(.middle) }
                .frame(height: 70)
                .border(Color.secondary.opacity(0.3))
            VStack {
                Button("Add Folder...") {
                    for p in pickFolders(title: pickTitle, multiple: true) where !folders.contains(p) { folders.append(p) }
                }
                Button("Remove Selected") {
                    folders.removeAll { selected.contains($0) }
                    selected = []
                }.disabled(selected.isEmpty)
            }
        }
    }
}
