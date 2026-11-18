import StorageScopeCore
import SwiftUI

struct OverviewView: View {
    @EnvironmentObject var model: AppModel

    private var disk: (total: Int64, free: Int64)? {
        guard let v = try? URL(fileURLWithPath: "/").resourceValues(forKeys: [.volumeTotalCapacityKey, .volumeAvailableCapacityForImportantUsageKey]),
              let t = v.volumeTotalCapacity, let f = v.volumeAvailableCapacityForImportantUsage else { return nil }
        return (Int64(t), f)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 20) {
                Text("StorageScope").font(.largeTitle.bold())
                Text("See what uses your disk, find big and forgotten files, and move them to the Trash safely.").foregroundStyle(.secondary)

                if let d = disk {
                    GroupBox {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Macintosh HD").font(.headline)
                            ProgressView(value: Double(d.total - d.free), total: Double(max(1, d.total)))
                            Text("\(Format.bytes(UInt64(max(0, d.total - d.free)))) used, \(Format.bytes(UInt64(max(0, d.free)))) available of \(Format.bytes(UInt64(max(0, d.total))))")
                                .font(.callout).foregroundStyle(.secondary)
                        }
                        .padding(6)
                    }
                }

                HStack(spacing: 12) {
                    Button { model.chooseFolderAndScan() } label: { Label("Scan Folder...", systemImage: "folder") }
                        .disabled(model.busy)
                    Button { model.scanWholeDisk() } label: { Label("Scan Whole Disk", systemImage: "internaldrive") }
                        .buttonStyle(.borderedProminent).disabled(model.busy)
                    if model.scanning { Button("Stop") { model.cancelScan() } }
                }
                .controlSize(.large)

                if model.scanning {
                    Text("A scan is running (see the bar above). Storage Analyzer and Treemap can already be browsed while it fills in.")
                        .foregroundStyle(.secondary)
                } else if let root = model.root {
                    GroupBox("Last scan: \(root.name)") {
                        VStack(alignment: .leading, spacing: 10) {
                            Text(model.summary).foregroundStyle(.secondary)
                            Divider()
                            Text("Biggest folders").font(.headline)
                            let top = root.childrenBySize(limit: 8)
                            ForEach(top) { c in
                                HStack {
                                    Image(systemName: c.isDir ? "folder" : "doc").frame(width: 18)
                                    Text(c.name).frame(width: 220, alignment: .leading).lineLimit(1)
                                    GeometryReader { g in
                                        RoundedRectangle(cornerRadius: 3).fill(Color.accentColor.opacity(0.7))
                                            .frame(width: g.size.width * CGFloat(Double(c.liveSize) / Double(max(1, root.liveSize))))
                                    }
                                    .frame(height: 10)
                                    Text(Format.bytes(c.liveSize)).monospacedDigit().frame(width: 90, alignment: .trailing)
                                }
                            }
                        }
                        .padding(6)
                    }
                    Text("Nothing is deleted without asking. Items go to the Trash, so they can be put back. System software (/System, /Library, /Applications) is never offered.")
                        .font(.callout).foregroundStyle(.secondary)
                }
            }
            .padding(24)
            .frame(maxWidth: 760, alignment: .leading)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
    }
}
