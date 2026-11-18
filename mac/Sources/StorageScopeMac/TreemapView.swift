import StorageScopeCore
import SwiftUI

/// Treemap of one folder at a time: click a folder to go into it, use the path bar to go back up.
struct TreemapView: View {
    @EnvironmentObject var model: AppModel
    @State private var current: Node?
    @State private var hover: Node?

    private var shown: Node? {
        if let c = current, !c.removed { return c }
        return model.root
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Button { if let p = shown?.parent { current = p } } label: { Image(systemName: "chevron.up") }
                    .disabled(shown?.parent == nil)
                    .help("Up one folder")
                Text(shown?.path ?? "").lineLimit(1).truncationMode(.head).font(.callout)
                Spacer()
                Text(shown.map { Format.bytes($0.liveSize) } ?? "").foregroundStyle(.secondary)
            }
            .padding(10)

            GeometryReader { geo in
                let kids = shown.map { Array($0.kids.filter { !$0.removed && !hiddenAppData($0) }.sorted { $0.liveSize > $1.liveSize }.prefix(300)) } ?? []
                let items = Treemap.layout(kids, in: CGRect(origin: .zero, size: geo.size))
                Canvas { ctx, _ in
                    for it in items {
                        let r = it.rect.insetBy(dx: 0.75, dy: 0.75)
                        ctx.fill(Path(roundedRect: r, cornerRadius: 3), with: .color(color(it.node, hovered: it.node === hover)))
                        if r.width > 80 && r.height > 36 {
                            ctx.draw(Text(it.node.name).font(.caption.bold()).foregroundColor(.white), at: CGPoint(x: r.minX + 6, y: r.minY + 5), anchor: .topLeading)
                            ctx.draw(Text(Format.bytes(it.node.liveSize)).font(.caption2).foregroundColor(.white.opacity(0.85)),
                                     at: CGPoint(x: r.minX + 6, y: r.minY + 21), anchor: .topLeading)
                        }
                    }
                }
                .contentShape(Rectangle())
                .onContinuousHover { phase in
                    switch phase {
                    case .active(let p): hover = items.first { $0.rect.contains(p) }?.node
                    case .ended: hover = nil
                    }
                }
                .gesture(DragGesture(minimumDistance: 0).onEnded { v in
                    if let n = items.first(where: { $0.rect.contains(v.location) })?.node, n.isDir, !n.kids.isEmpty { current = n }
                })
                .contextMenu {
                    if let n = hover {
                        Button("Show in Finder") { model.showInFinder(n) }
                        Button("Move to Trash...") { model.confirmAndTrash([n]) }.disabled(model.rules.classify(n.path) == .blocked)
                    }
                }
            }
            .padding(.horizontal, 10)

            Text(hover.map { "\($0.name)  -  \(Format.bytes($0.liveSize))\($0.isDir ? "  -  click to open" : "")" } ?? "Hover over a block. Click a folder to open it.")
                .font(.callout).foregroundStyle(.secondary).lineLimit(1).padding(10)
        }
        .onChange(of: model.revision) { _ in if current?.removed == true { current = nil } }
        .onChange(of: model.root?.id) { _ in current = nil }  // a different tree (new scan, stopped scan): start again at its top
    }

    private func hiddenAppData(_ n: Node) -> Bool { !model.showAppData && n.isDir && model.rules.isAppData(n.path) }

    private func color(_ n: Node, hovered: Bool) -> Color {
        var h = 0
        let key = n.isDir ? n.name : String(FileCategory.of(name: n.name).rawValue)
        for b in key.utf8 { h = (h &* 31 &+ Int(b)) & 0xFFFF }  // stable between launches (hashValue is not)
        let hue = Double(h % 360) / 360
        return Color(hue: hue, saturation: n.isDir ? 0.5 : 0.35, brightness: hovered ? 0.85 : 0.68)
    }
}
