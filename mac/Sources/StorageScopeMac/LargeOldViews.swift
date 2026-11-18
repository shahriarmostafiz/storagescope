import StorageScopeCore
import SwiftUI

private let mb: UInt64 = 1_000_000

struct LargeFilesView: View {
    @EnvironmentObject var model: AppModel
    @AppStorage("largeMinMB") private var minMB = 100
    @State private var typeIdx = 0  // 0 = all types
    @State private var rows: [Node] = []
    @State private var token = 0
    @State private var busy = false

    private var key: String { "\(minMB)-\(typeIdx)-\(model.revision)-\(model.showAppData)" }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 14) {
                Picker("Bigger than", selection: $minMB) {
                    ForEach([10, 50, 100, 250, 500, 1000], id: \.self) { Text($0 >= 1000 ? "1 GB" : "\($0) MB").tag($0) }
                }.frame(width: 190)
                Picker("Type", selection: $typeIdx) {
                    Text("All types").tag(0)
                    ForEach(FileCategory.allCases, id: \.rawValue) { Text($0.title).tag($0.rawValue + 1) }
                }.frame(width: 230)
                if busy { ProgressView().controlSize(.small) }
                Spacer()
            }
            .padding(12)
            ResultsPanel(rows: rows, token: token, info: "\(rows.count) files", emptyText: "No files this big. Try a smaller size.")
        }
        .task(id: key) { await load() }
    }

    @MainActor private func load() async {
        guard let root = model.root else { return }
        busy = true
        let skip = model.skip(model.showAppData)
        let min = UInt64(minMB) * mb
        let cat: FileCategory? = typeIdx > 0 ? FileCategory(rawValue: typeIdx - 1) : nil
        let found = await Task.detached(priority: .userInitiated) { Insights.largeFiles(root: root, minSize: min, category: cat, skip: skip) }.value
        rows = found
        token += 1
        busy = false
    }
}

struct OldFilesView: View {
    @EnvironmentObject var model: AppModel
    @AppStorage("oldMonths") private var months = 3
    @AppStorage("oldMinMB") private var minMB = 10
    @State private var rows: [Node] = []
    @State private var token = 0
    @State private var busy = false

    private var key: String { "\(months)-\(minMB)-\(model.revision)-\(model.showAppData)" }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 14) {
                Picker("Not modified for", selection: $months) {
                    ForEach([1, 3, 6, 12, 24], id: \.self) { Text($0 == 12 ? "1 year" : $0 == 24 ? "2 years" : "\($0) month\($0 == 1 ? "" : "s")").tag($0) }
                }.frame(width: 260)
                Picker("Bigger than", selection: $minMB) {
                    ForEach([1, 10, 100, 1000], id: \.self) { Text($0 >= 1000 ? "1 GB" : "\($0) MB").tag($0) }
                }.frame(width: 190)
                if busy { ProgressView().controlSize(.small) }
                Spacer()
                Image(systemName: "info.circle")
                    .help("Age is the last-modified date. macOS does not reliably record when a file was last opened. A folder whose files are all old is listed as one row.")
            }
            .padding(12)
            ResultsPanel(rows: rows, token: token, info: "\(rows.count) items", emptyText: "Nothing this old. Try a shorter time or a smaller size.")
        }
        .task(id: key) { await load() }
    }

    @MainActor private func load() async {
        guard let root = model.root else { return }
        busy = true
        let skip = model.skip(model.showAppData)
        let cutoff = Calendar.current.date(byAdding: .month, value: -months, to: Date())?.timeIntervalSince1970 ?? 0
        let min = UInt64(minMB) * mb
        let found = await Task.detached(priority: .userInitiated) { Insights.oldItems(root: root, cutoff: cutoff, minSize: min, skip: skip) }.value
        rows = found
        token += 1
        busy = false
    }
}
