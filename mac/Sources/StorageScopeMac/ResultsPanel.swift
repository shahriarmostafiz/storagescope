import StorageScopeCore
import SwiftUI

/// The list used by Large Files, Old Files and Search: sortable table, multi-select, "Select all",
/// and the actions (Show in Finder, Move to Trash).
struct ResultsPanel: View {
    @EnvironmentObject var model: AppModel
    let rows: [Node]
    let token: Int          // changes when `rows` was replaced
    let info: String
    let emptyText: String
    var rankQuery = ""      // Search: the query these rows answer. Non-empty = best match first, until the user clicks a column header

    @State private var sorted: [Node] = []
    @State private var userSorted = false
    @State private var ignoreNextSortChange = false
    @State private var selection = Set<Node.ID>()
    @State private var sortOrder: [KeyPathComparator<Node>] = [KeyPathComparator(\Node.size, order: .reverse)]

    private var selectedNodes: [Node] { sorted.filter { selection.contains($0.id) } }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Text(info).font(.callout).foregroundStyle(.secondary).lineLimit(1)
                Spacer()
                Button(allSelected ? "Clear selection" : "Select all") {
                    selection = allSelected ? [] : Set(sorted.map { $0.id })
                }
                .disabled(sorted.isEmpty)
            }
            .padding(.horizontal, 12).padding(.vertical, 8)

            if sorted.isEmpty {
                Text(emptyText).foregroundStyle(.secondary).multilineTextAlignment(.center).padding(40)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                Table(sorted, selection: $selection, sortOrder: $sortOrder) {
                    TableColumn("Name", value: \.name) { n in
                        HStack(spacing: 6) {
                            Image(systemName: n.isDir ? "folder.fill" : "doc").foregroundStyle(n.isDir ? Color.accentColor : .secondary)
                            Text(n.name).lineLimit(1)
                        }
                    }
                    TableColumn("Size", value: \.size) { n in Text(Format.bytes(n.size)).monospacedDigit() }
                        .width(min: 70, ideal: 90, max: 120)
                    TableColumn("Type") { n in Text(n.isDir ? "Folder" : FileCategory.of(name: n.name).title) }
                        .width(min: 70, ideal: 110, max: 150)
                    TableColumn("Location") { n in Text(n.location).lineLimit(1).truncationMode(.head).foregroundStyle(.secondary) }
                    TableColumn("Modified", value: \.modified) { n in Text(Format.date(n.modified)).foregroundStyle(.secondary) }
                        .width(min: 110, ideal: 140, max: 180)
                }
                .contextMenu(forSelectionType: Node.ID.self) { ids in
                    let nodes = sorted.filter { ids.contains($0.id) }
                    if nodes.count == 1 { Button("Show in Finder") { model.showInFinder(nodes[0]) } }
                    Button("Move to Trash...") { model.confirmAndTrash(nodes) }.disabled(!model.canTrash(nodes))
                }
            }

            Divider()
            HStack {
                let sel = selectedNodes
                Text(sel.isEmpty ? "Select items to act on them" : "\(sel.count) selected, \(Format.bytes(sel.reduce(UInt64(0)) { $0 + $1.size }))")
                    .font(.callout).foregroundStyle(.secondary)
                Spacer()
                Button("Show in Finder") { if let n = sel.first { model.showInFinder(n) } }.disabled(sel.count != 1)
                Button("Move to Trash...") { model.confirmAndTrash(sel) }
                    .disabled(sel.isEmpty || !model.canTrash(sel))
                    .keyboardShortcut(.delete, modifiers: .command)
            }
            .padding(10)
        }
        .onAppear {
            normalizeOrder()
            resort()
        }
        .onChange(of: token) { _ in
            selection = selection.intersection(Set(rows.map { $0.id }))
            normalizeOrder()
            resort()
        }
        .onChange(of: sortOrder) { _ in
            if ignoreNextSortChange { ignoreNextSortChange = false } else { userSorted = true }  // a click on a column header
            resort()
        }
    }

    private var allSelected: Bool { !sorted.isEmpty && selection.count == sorted.count }

    /// Best match first (no column arrow) while a search query is active and nobody picked a column; otherwise the size order.
    private func normalizeOrder() {
        if userSorted { return }
        if !rankQuery.isEmpty {
            if !sortOrder.isEmpty {
                ignoreNextSortChange = true
                sortOrder = []
            }
        } else if sortOrder.isEmpty {
            ignoreNextSortChange = true
            sortOrder = [KeyPathComparator(\Node.size, order: .reverse)]
        }
    }

    private func resort() {
        if !userSorted && !rankQuery.isEmpty {
            sorted = SearchRank.order(rows, query: rankQuery)
            return
        }
        var order = sortOrder
        if order.isEmpty { order = [KeyPathComparator(\Node.size, order: .reverse)] }
        sorted = rows.sorted(using: order)
    }
}
