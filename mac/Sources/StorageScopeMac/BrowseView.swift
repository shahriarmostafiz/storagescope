import StorageScopeCore
import SwiftUI

/// Storage Analyzer: the folder tree, biggest first, with a bar showing each item's share of its folder.
/// During a scan it shows the tree as far as it has been scanned and follows it about once a second (folders keep their open/closed state).
struct BrowseView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        if let root = model.root {
            List {
                TreeRow(node: root, share: root.liveSize, startOpen: true)
            }
        }
    }
}

private struct TreeRow: View {
    @EnvironmentObject var model: AppModel
    let node: Node
    let share: UInt64
    var startOpen = false
    @State private var open = false

    private var visibleChildren: [Node] {
        let hideAppData = !model.showAppData
        let all = node.kids.filter { !$0.removed && !(hideAppData && $0.isDir && model.rules.isAppData($0.path)) }
        let sized = all.map { ($0, $0.liveSize) }  // sizes read once: a live scan keeps adding to them
        return Array(sized.sorted { $0.1 > $1.1 }.prefix(1000).map { $0.0 })  // a folder with 100,000 items would make the list crawl
    }

    var body: some View {
        if node.isDir && !node.kids.isEmpty {
            DisclosureGroup(isExpanded: $open) {
                if open {
                    ForEach(visibleChildren) { c in TreeRow(node: c, share: max(1, node.liveSize)) }
                }
            } label: {
                label
            }
            .onAppear { if startOpen { open = true } }
        } else {
            label
        }
    }

    private var label: some View {
        HStack(spacing: 8) {
            Image(systemName: node.isDir ? "folder.fill" : "doc").foregroundStyle(node.isDir ? Color.accentColor : .secondary).frame(width: 18)
            Text(node.name).lineLimit(1).truncationMode(.middle)
            if node.unreadable { Image(systemName: "lock.fill").foregroundStyle(.orange).help("Could not be read") }
            if model.live && node.isBuilding { Text("scanning...").font(.caption).foregroundStyle(.secondary) }
            Spacer()
            GeometryReader { g in
                RoundedRectangle(cornerRadius: 3).fill(Color.accentColor.opacity(0.55))
                    .frame(width: g.size.width * CGFloat(Double(node.liveSize) / Double(max(1, share))))
            }
            .frame(width: 120, height: 8)
            Text(Format.bytes(node.liveSize)).monospacedDigit().frame(width: 84, alignment: .trailing)
        }
        .contentShape(Rectangle())
        .contextMenu {
            Button("Show in Finder") { model.showInFinder(node) }
            if node.parent != nil {
                Button("Move to Trash...") { model.confirmAndTrash([node]) }
                    .disabled(!model.canTrash([node]))
            }
        }
    }
}
