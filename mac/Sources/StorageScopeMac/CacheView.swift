import StorageScopeCore
import SwiftUI

/// Cache, logs and build data inside the user's own Library. Everything goes to the Trash, nothing is deleted for good.
struct CacheView: View {
    @EnvironmentObject var model: AppModel

    struct Row: Identifiable {
        let cat: CacheCategory
        var scanned = false
        var files = 0
        var bytes: UInt64 = 0
        var entries: [String] = []
        var checked = true
        var id: String { cat.id }
    }

    @State private var rows: [Row] = []
    @State private var ageIdx = 1
    @State private var working = false
    @State private var note = ""
    private let ages: [(String, Double)] = [("Everything", 0), ("Not used for 1 day", 86_400), ("Not used for 7 days", 7 * 86_400), ("Not used for 30 days", 30 * 86_400)]

    private var scannedRows: [Row] { rows.filter { $0.scanned } }
    private var allChecked: Bool { !scannedRows.isEmpty && scannedRows.allSatisfy { $0.checked } }
    private var selectedBytes: UInt64 { scannedRows.filter { $0.checked }.reduce(0) { $0 + $1.bytes } }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Button { Task { await scan() } } label: { Label("Scan Cache & Logs", systemImage: "magnifyingglass") }
                    .buttonStyle(.borderedProminent).disabled(working)
                Picker("Clean", selection: $ageIdx) {
                    ForEach(0..<ages.count, id: \.self) { Text(ages[$0].0).tag($0) }
                }.frame(width: 260)
                .onChange(of: ageIdx) { _ in
                    for i in rows.indices { rows[i].scanned = false }
                    note = "Filter changed. Scan again to see what can be cleaned."
                }
                if working { ProgressView().controlSize(.small) }
                Spacer()
            }
            .padding(12)

            if rows.isEmpty {
                Text("No known cache folders were found.").foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                List {
                    ForEach($rows) { $r in
                        HStack(spacing: 10) {
                            Toggle("", isOn: $r.checked).labelsHidden().disabled(!r.scanned)
                            VStack(alignment: .leading, spacing: 2) {
                                Text(r.cat.name)
                                Text(r.cat.path).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                            }
                            Spacer()
                            Text(r.scanned ? Format.count(r.files) + " files" : "-").foregroundStyle(.secondary).frame(width: 110, alignment: .trailing)
                            Text(r.scanned ? Format.bytes(r.bytes) : "-").monospacedDigit().frame(width: 90, alignment: .trailing)
                            Text(r.cat.risk.rawValue).foregroundStyle(r.cat.risk == .medium ? Color.orange : Color.secondary).frame(width: 64, alignment: .trailing)
                        }
                    }
                }
            }

            Divider()
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(scannedRows.isEmpty ? "Recoverable space: not scanned yet" : "Recoverable space: \(Format.bytes(selectedBytes))").font(.headline)
                    Spacer()
                    Button(allChecked ? "Clear selection" : "Select all") {
                        let v = !allChecked
                        for i in rows.indices where rows[i].scanned { rows[i].checked = v }
                    }.disabled(scannedRows.isEmpty || working)
                    Button("Move Selected to Trash...") { clean() }
                        .disabled(working || scannedRows.filter { $0.checked && $0.bytes > 0 }.isEmpty)
                }
                Text(note.isEmpty ? "Items go to the Trash. Empty the Trash in Finder to get the space back. Browser caches rebuild themselves (pages load slower once)." : note)
                    .font(.callout).foregroundStyle(.secondary)
            }
            .padding(12)
        }
        .onAppear { if rows.isEmpty { rows = CacheCleaner.discover().map { Row(cat: $0) } } }
    }

    @MainActor private func scan() async {
        working = true
        note = ""
        let cats = rows.map { $0.cat }
        let age = ages[ageIdx].1
        let measured = await Task.detached(priority: .userInitiated) { cats.map { CacheCleaner.measure($0, olderThan: age) } }.value
        for (i, m) in measured.enumerated() where i < rows.count {
            rows[i].scanned = true
            rows[i].files = m.files
            rows[i].bytes = m.bytes
            rows[i].entries = m.entries
            rows[i].checked = m.bytes > 0
        }
        working = false
    }

    @MainActor private func clean() {
        let chosen = scannedRows.filter { $0.checked && $0.bytes > 0 }
        let paths = chosen.flatMap { $0.entries }.filter { CacheCleaner.isValid($0) }  // only items inside the known cache folders
        guard !paths.isEmpty else { return }
        let a = NSAlert()
        a.messageText = "Move \(Format.bytes(chosen.reduce(0) { $0 + $1.bytes })) of cache and logs to the Trash?"
        a.informativeText = "Close the apps whose caches you are cleaning first. You can put the items back from the Trash."
        a.addButton(withTitle: "Move to Trash")
        a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { return }
        working = true
        let rules = model.rules
        Task {
            let res = await Task.detached(priority: .userInitiated) { Trasher.trash(paths: paths, rules: rules) }.value
            note = "Moved \(res.trashed.count) item(s) to the Trash." + (res.failed.isEmpty ? "" : " \(res.failed.count) could not be moved (in use or protected).")
            working = false
            await scan()
        }
    }
}
