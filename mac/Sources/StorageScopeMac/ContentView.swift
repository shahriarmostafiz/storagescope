import StorageScopeCore
import SwiftUI

struct ContentView: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        NavigationSplitView {
            List(selection: $model.selectedPage) {
                ForEach(Page.allCases) { p in
                    Label(p.rawValue, systemImage: p.icon).tag(p)
                }
                Section {
                    Toggle("Show app data", isOn: $model.showAppData)
                        .help("Library and tool folders such as .config and .npm are hidden from lists by default.")
                        .toggleStyle(.switch)
                        .controlSize(.small)
                    Toggle("Remember last scan", isOn: $model.rememberScan)
                        .help("Saves the names and sizes of the last scan on this Mac so the next launch starts with data.")
                        .toggleStyle(.switch)
                        .controlSize(.small)
                    if model.rememberScan {
                        Button("Clear saved scan") { model.clearSavedScan() }.controlSize(.small)
                    }
                }
            }
            .navigationSplitViewColumnWidth(min: 190, ideal: 210)
        } detail: {
            VStack(spacing: 0) {
                ScanBar()
                if model.unreadable > 0 && !model.scanning { AccessBanner(count: model.unreadable) }
                detail
            }
        }
    }

    @ViewBuilder private var detail: some View {
        switch model.page {
        case .overview: OverviewView()
        case .browse: NeedsScan { BrowseView() }
        case .search: NeedsScan(finished: true) { SearchView() }
        case .treemap: NeedsScan { TreemapView() }
        case .large: NeedsScan(finished: true) { LargeFilesView() }
        case .old: NeedsScan(finished: true) { OldFilesView() }
        case .duplicates: NeedsScan(finished: true) { DuplicatesView() }
        case .cache: CacheView()
        }
    }
}

/// Shown above every page while a scan runs: the percentage and the time left. "View more" adds the folder being scanned and the totals.
struct ScanBar: View {
    @EnvironmentObject var model: AppModel

    var body: some View {
        if model.scanning || model.saving || model.loading {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(title).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    if model.scanning {
                        Button(model.showDetails ? "View less" : "View more") { model.showDetails.toggle() }
                        Button("Stop Scan") { model.cancelScan() }
                    }
                }
                if model.scanning {
                    if let p = model.percent {
                        ProgressView(value: Double(p), total: 100)
                        HStack(spacing: 18) {
                            Text("Progress: \(p)%")
                            Text(model.timeLeft)
                        }
                        .font(.callout)
                    } else {
                        ProgressView().progressViewStyle(.linear)
                        Text("A percentage is shown when the whole disk is scanned.").font(.callout).foregroundStyle(.secondary)
                    }
                    if model.showDetails {
                        Text(model.scanStats).font(.callout).foregroundStyle(.secondary)
                    }
                } else {
                    ProgressView().progressViewStyle(.linear)
                }
            }
            .padding(10)
            .background(Color.secondary.opacity(0.08))
        }
    }

    private var title: String {
        if model.scanning { return model.showDetails ? "Scanning folder: " + model.progress : "Scanning..." }
        return model.saving ? "Saving the scan..." : "Loading the saved scan..."
    }
}

/// Pages that read the scan show this until a scan exists.
/// `finished`: the page needs the completed scan; while one is still filling in, the page waits (Storage Analyzer and Treemap can browse it live).
struct NeedsScan<Content: View>: View {
    @EnvironmentObject var model: AppModel
    var finished = false
    @ViewBuilder var content: () -> Content

    var body: some View {
        if model.root != nil && !(finished && model.live) {
            content()
        } else {
            VStack(spacing: 14) {
                Image(systemName: waiting ? "hourglass" : "externaldrive.badge.questionmark").font(.system(size: 44)).foregroundStyle(.secondary)
                Text(headline).font(.title2)
                if model.live {
                    Text("This page fills in when the scan finishes. Storage Analyzer and Treemap can already be browsed.")
                        .foregroundStyle(.secondary).multilineTextAlignment(.center).padding(.horizontal, 40)
                } else if model.scanning {
                    Text(model.progress).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle).padding(.horizontal, 40)
                } else if !model.loading {
                    Text("Scan a folder or the whole disk first.").foregroundStyle(.secondary)
                    HStack {
                        Button("Scan Folder...") { model.chooseFolderAndScan() }
                        Button("Scan Whole Disk") { model.scanWholeDisk() }.buttonStyle(.borderedProminent)
                    }
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
    }

    private var waiting: Bool { model.scanning || model.loading }

    private var headline: String {
        if model.loading { return "Loading the saved scan..." }
        return model.scanning ? "Scanning..." : "No scan yet"
    }
}

/// Without Full Disk Access macOS refuses to list some folders (Mail, Safari, Messages ...).
struct AccessBanner: View {
    let count: Int

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "lock.shield").foregroundStyle(.orange)
            Text("\(count) folder\(count == 1 ? "" : "s") could not be read, so totals are lower than the real disk use. Give StorageScope Full Disk Access to include them.")
                .font(.callout)
            Spacer()
            Button("Open Privacy Settings") {
                if let u = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_AllFiles") { NSWorkspace.shared.open(u) }
            }
        }
        .padding(10)
        .background(Color.orange.opacity(0.12))
    }
}
