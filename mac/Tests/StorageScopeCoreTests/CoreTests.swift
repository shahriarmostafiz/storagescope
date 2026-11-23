import XCTest
@testable import StorageScopeCore

final class CoreTests: XCTestCase {
    var dir = ""

    override func setUpWithError() throws {
        dir = NSTemporaryDirectory() + "StorageScopeTests-" + UUID().uuidString
        let fm = FileManager.default
        try fm.createDirectory(atPath: dir + "/docs/old", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: dir + "/media", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: dir + "/.hidden", withIntermediateDirectories: true)
        func file(_ p: String, _ bytes: Int) { fm.createFile(atPath: dir + "/" + p, contents: Data(count: bytes)) }
        file("docs/Report 2025.pdf", 10_000)
        file("docs/notes.txt", 100)
        file("docs/old/archive.zip", 50_000)
        file("media/photo.JPG", 20_000)
        file("media/clip.mov", 200_000)
        file(".hidden/secret.txt", 100)
        // make docs/old look old
        let old = Date(timeIntervalSinceNow: -400 * 86_400)
        try fm.setAttributes([.modificationDate: old], ofItemAtPath: dir + "/docs/old/archive.zip")
    }

    override func tearDownWithError() throws { try? FileManager.default.removeItem(atPath: dir) }

    private func scan() -> Node { DiskScanner.scan(path: dir) }

    func testScanTotalsAndCounts() {
        let root = scan()
        XCTAssertEqual(root.files, 6)
        XCTAssertGreaterThanOrEqual(root.size, 280_000)  // allocated bytes are never below the real size
        XCTAssertEqual(root.find(path: dir + "/docs")?.files, 3)
        XCTAssertEqual(root.find(path: dir + "/DOCS/old/ARCHIVE.zip")?.isDir, false)  // case-insensitive
        XCTAssertNil(root.find(path: dir + "/nope"))
    }

    func testDetachFixesTotals() {
        let root = scan()
        let before = root.size
        let clip = root.find(path: dir + "/media/clip.mov")!
        let clipSize = clip.size
        clip.detach()
        XCTAssertEqual(root.size, before - clipSize)
        XCTAssertEqual(root.files, 5)
        XCTAssertNil(root.find(path: dir + "/media/clip.mov"))
    }

    func testSearchWordsPhraseGlobAndKinds() {
        let root = scan()
        let idx = SearchIndex(root: root)
        func names(_ f: (inout SearchSpec) -> Void) -> [String] {
            var s = SearchSpec()
            f(&s)
            return idx.run(s).hits.map { $0.name }.sorted()
        }
        XCTAssertEqual(names { $0.text = "report 2025" }, ["Report 2025.pdf"])
        XCTAssertEqual(names { $0.text = "\"report 2025\"" }, ["Report 2025.pdf"])
        XCTAssertEqual(names { $0.text = "report 2026" }, [])
        XCTAssertEqual(names { $0.text = "*.jpg" }, ["photo.JPG"])
        XCTAssertEqual(names { $0.text = "PHOTO"; $0.matchCase = true }, [])
        XCTAssertEqual(names { $0.text = "o"; $0.kind = .foldersOnly }, ["docs", "old"])
        XCTAssertEqual(names { $0.text = "secret" }, [])                            // dot-files and dot-folders are hidden by default
        XCTAssertEqual(names { $0.text = "secret"; $0.showHidden = true }, ["secret.txt"])
    }

    func testSearchFilters() {
        let root = scan()
        let idx = SearchIndex(root: root)
        var s = SearchSpec()
        s.include = "pdf, zip"
        XCTAssertEqual(idx.run(s).hits.map { $0.name }.sorted(), ["Report 2025.pdf", "archive.zip"])
        s = SearchSpec()
        s.category = .video
        XCTAssertEqual(idx.run(s).hits.map { $0.name }, ["clip.mov"])
        s = SearchSpec()
        s.minSize = 100_000
        s.kind = .filesOnly
        XCTAssertEqual(idx.run(s).hits.map { $0.name }, ["clip.mov"])
        s = SearchSpec()
        s.text = "["
        s.regex = true
        XCTAssertNotNil(idx.run(s).error)
        s = SearchSpec()
        s.scopes = [root.find(path: dir + "/media")!]
        XCTAssertEqual(idx.run(s).hits.map { $0.name }.sorted(), ["clip.mov", "photo.JPG"])
        s = SearchSpec()
        s.excluded = [root.find(path: dir + "/media")!]
        XCTAssertFalse(idx.run(s).hits.contains { $0.name == "clip.mov" })
    }

    func testOldItemsListsWholeOldFolderOnce() {
        let root = scan()
        let cutoff = Date(timeIntervalSinceNow: -90 * 86_400).timeIntervalSince1970
        let items = Insights.oldItems(root: root, cutoff: cutoff, minSize: 0)
        XCTAssertEqual(items.map { $0.name }, ["old"])  // the folder, not archive.zip separately
    }

    func testLargeFiles() {
        let root = scan()
        let big = Insights.largeFiles(root: root, minSize: 100_000)
        XCTAssertEqual(big.map { $0.name }, ["clip.mov"])
    }

    func testProtection() {
        let r = ProtectionRules(home: "/Users/ann")
        XCTAssertEqual(r.classify("/"), .blocked)
        XCTAssertEqual(r.classify("/System/Library/x"), .blocked)
        XCTAssertEqual(r.classify("/Applications/Safari.app"), .blocked)
        XCTAssertEqual(r.classify("/Users/ann"), .blocked)
        XCTAssertEqual(r.classify("/Users/ann/Documents"), .blocked)
        XCTAssertEqual(r.classify("/Users/ann/Documents/a.txt"), .normal)
        XCTAssertEqual(r.classify("/Users/ann/Library/Caches/x"), .caution)
        XCTAssertEqual(r.classify("/Users/ann/.ssh/id_rsa"), .blocked)
        XCTAssertEqual(r.classify("/Users/ann/.npm/_cacache"), .caution)
        XCTAssertTrue(r.isAppData("/Users/ann/Library"))
        XCTAssertTrue(r.isAppData("/Users/ann/.config"))
        XCTAssertFalse(r.isAppData("/Users/ann/Documents"))
        XCTAssertFalse(r.isAppData("/Users/ann/Documents/.git"))
    }

    func testTreemapAreasAddUp() {
        let p = Node(name: "/", isDir: true)
        let kids = [900, 500, 300, 200, 100, 50].map { Node(name: "n\($0)", isDir: false, size: UInt64($0), files: 1, parent: p) }
        let rect = CGRect(x: 0, y: 0, width: 400, height: 300)
        let items = Treemap.layout(kids, in: rect)
        XCTAssertEqual(items.count, 6)
        let area = items.reduce(0.0) { $0 + Double($1.rect.width * $1.rect.height) }
        XCTAssertEqual(area, Double(rect.width * rect.height), accuracy: 1.0)
        for it in items { XCTAssertTrue(rect.insetBy(dx: -0.01, dy: -0.01).contains(it.rect)) }
    }

    func testTrashRefusesProtectedPaths() {
        let res = Trasher.trash(paths: ["/System/Library", "/Applications"], rules: ProtectionRules())
        XCTAssertEqual(res.trashed.count, 0)
        XCTAssertEqual(res.failed.count, 2)
    }

    func testSearchRankPutsExactNamesFirst() {
        let p = Node(name: "/", isDir: true)
        func f(_ n: String) -> Node { Node(name: n, isDir: false, size: 1, files: 1, parent: p) }
        let list = [f("readme.md.bak"), f("README"), f("my readme file.txt"), f("readme.txt"), f("xreadmex.txt"), f("notes.txt")]
        let order = SearchRank.order(list, query: "readme").map { $0.name }
        XCTAssertEqual(order, ["README", "readme.txt", "readme.md.bak", "my readme file.txt", "xreadmex.txt", "notes.txt"])
        XCTAssertEqual(SearchRank.order(list, query: "").count, list.count)  // no query: unchanged
    }

    func testRegexGuardRefusesNestedRepeats() {
        for bad in ["(a+)+$", "(.*x)*", "((a|b)+c)+", "(\\w+\\s?)*"] { XCTAssertTrue(SearchIndex.repeatsARepeat(bad), bad) }
        for good in ["^re.*pdf$", "(jpg|png)$", "(\\d{4}-\\d{2})+", "(ab){2,3}c", "\\(a+\\)", "[(+*]x"] { XCTAssertFalse(SearchIndex.repeatsARepeat(good), good) }
        let root = scan()
        var spec = SearchSpec()
        spec.regex = true
        spec.text = "(a+)+$"
        XCTAssertNotNil(SearchIndex(root: root).run(spec).error)
    }

    func testSnapshotRoundTripAndDamagedFile() throws {
        let root = scan()
        let url = URL(fileURLWithPath: NSTemporaryDirectory() + "snap-" + UUID().uuidString + ".bin")
        defer { try? FileManager.default.removeItem(at: url) }
        XCTAssertTrue(Snapshot.save(root: root, to: url))
        let loaded = Snapshot.load(from: url)
        XCTAssertNotNil(loaded)
        XCTAssertEqual(loaded?.root.size, root.size)
        XCTAssertEqual(loaded?.root.files, root.files)
        XCTAssertEqual(loaded?.root.find(path: dir + "/docs")?.files, 3)
        XCTAssertEqual(loaded?.root.find(path: dir + "/media/clip.mov")?.size, root.find(path: dir + "/media/clip.mov")?.size)
        // a damaged or foreign file is rejected, never trusted
        var data = try Data(contentsOf: url)
        data.removeLast(data.count / 2)
        try data.write(to: url)
        XCTAssertNil(Snapshot.load(from: url))
        XCTAssertNil(Snapshot.load(from: URL(fileURLWithPath: "/nonexistent/folder/file.bin")))
    }

    func testLiveScanPublishesFoldersAndEndsExact() {
        var sawEmpty = false
        let root = DiskScanner.scan(path: dir, onRoot: { r in sawEmpty = r.kids.isEmpty && r.isBuilding })
        XCTAssertTrue(sawEmpty)  // nothing is readable before the first folder is listed
        XCTAssertFalse(root.isBuilding)
        XCTAssertEqual(root.kids.count, 3)  // docs, media, .hidden
        XCTAssertEqual(root.liveSize, root.size)
        func anyBuilding(_ n: Node) -> Bool { n.isBuilding || n.children.contains(where: anyBuilding) }
        XCTAssertFalse(anyBuilding(root))  // every folder was published
    }

    func testDurationFormat() {
        XCTAssertEqual(Format.duration(75), "1:15")
        XCTAssertEqual(Format.duration(3725), "1:02:05")
        XCTAssertEqual(Format.duration(-5), "0:00")
    }

    func testEtaEstimator() {
        // A disk of 100,000 files scanned at 1,000 a second; after 30 s it slows to 100 a second (a stretch of tiny files).
        var e = EtaEstimator(cautious: false)
        var done = 0.0, t = 0.0, prev = 0.0, maxFall = 0.0, sawNotReady = false
        var finished = false
        while t <= 800 {
            done += (t < 30 ? 1000 : 100) * 0.1
            t += 0.1
            let r = e.update(elapsed: t, done: done, total: 100_000)
            if done >= 100_000 { finished = true; break }
            if !r.ready { sawNotReady = sawNotReady || t < 6; continue }
            if prev > 0 { maxFall = max(maxFall, prev - r.seconds - 0.1 - 1e-6) }  // it may fall by the 0.1 s that passed, no more
            prev = r.seconds
        }
        XCTAssertTrue(sawNotReady)      // nothing is promised in the first seconds
        XCTAssertLessThan(maxFall, 0.01)  // the countdown never falls faster than the clock
        XCTAssertTrue(finished)
        // steady speed: the estimate is close to the truth (80 s left after 20 s)
        var s = EtaEstimator(cautious: false)
        var r = EtaEstimator.Result()
        var ts = 0.1
        while ts <= 20 { r = s.update(elapsed: ts, done: ts * 1000, total: 100_000); ts += 0.1 }
        XCTAssertTrue(r.ready && r.seconds > 70 && r.seconds < 90)
        // a first scan (cautious) promises more time than the same feed with a known total: fast start, then a slow stretch
        var c = EtaEstimator(cautious: true), n = EtaEstimator(cautious: false)
        var rc = EtaEstimator.Result(), rn = EtaEstimator.Result()
        var tc = 0.1
        while tc <= 10 {
            let d = tc < 5 ? tc * 10_000 : 50_000 + (tc - 5) * 1000
            rc = c.update(elapsed: tc, done: d, total: 1_000_000)
            rn = n.update(elapsed: tc, done: d, total: 1_000_000)
            tc += 0.1
        }
        XCTAssertTrue(rc.ready && rn.ready && rc.seconds > rn.seconds)
    }

    func testDuplicateFinder() throws {
        let fm = FileManager.default
        let d = dir + "/dups"
        try fm.createDirectory(atPath: d + "/a", withIntermediateDirectories: true)
        try fm.createDirectory(atPath: d + "/b", withIntermediateDirectories: true)
        let big = Data(repeating: 0x61, count: 300 * 1024)
        var tail = big, mid = big
        tail[tail.count - 1] = 0x62      // differs only at the very end: the quick check tells it apart
        mid[150 * 1024] = 0x62           // differs only in the middle: only the full comparison tells it apart
        func put(_ p: String, _ data: Data) { fm.createFile(atPath: d + "/" + p, contents: data) }
        put("a/one.bin", big); put("b/two.bin", big); put("b/three.bin", big)
        put("a/tail.bin", tail); put("a/mid.bin", mid)
        put("a/small1.txt", Data(repeating: 0x63, count: 1000)); put("b/small2.txt", Data(repeating: 0x63, count: 1000))
        put("a/empty1", Data()); put("b/empty2", Data())
        put("a/unique.txt", Data("something else entirely".utf8))
        func age(_ p: String, _ days: Double) throws {
            try fm.setAttributes([.modificationDate: Date(timeIntervalSinceNow: -days * 86_400)], ofItemAtPath: d + "/" + p)
        }
        try age("a/one.bin", 30); try age("b/three.bin", 20); try age("b/two.bin", 10)  // the oldest is the one to keep
        try age("a/small1.txt", 5); try age("b/small2.txt", 1)

        let root = scan()
        let files = DuplicateFinder.candidates(root: root.find(path: d)!, minSize: 1)
        XCTAssertEqual(files.count, 8)  // the two empty files are not candidates
        // a hard link is a second name of the same file, not a copy; a symbolic link is not a file to compare at all
        try fm.createDirectory(atPath: d + "/links", withIntermediateDirectories: true)
        put("links/orig.bin", big)
        try fm.linkItem(atPath: d + "/links/orig.bin", toPath: d + "/links/hard.bin")
        try fm.createSymbolicLink(atPath: d + "/links/sym.bin", withDestinationPath: d + "/links/orig.bin")
        try fm.setAttributes([.modificationDate: Date(timeIntervalSinceNow: -50 * 86_400)], ofItemAtPath: d + "/links/orig.bin")  // the oldest of all
        let withLinks = DuplicateFinder.candidates(root: scan().find(path: d)!, minSize: 1)
        let linked = DuplicateFinder.find(withLinks, threads: 2)
        let names = linked.first?.map { $0.node.name } ?? []
        XCTAssertEqual(names.count, 4)  // one name of the linked file, plus one.bin, two.bin and three.bin
        XCTAssertEqual(Array(names.dropFirst()), ["one.bin", "three.bin", "two.bin"])
        XCTAssertTrue(["orig.bin", "hard.bin"].contains(names.first ?? ""))  // either name of the oldest file, once
        XCTAssertFalse(names.contains("sym.bin"))
        for threads in [1, 4] {
            let progress = DuplicateFinder.Progress()
            let groups = DuplicateFinder.find(files, threads: threads, progress: progress)
            XCTAssertEqual(groups.count, 2)
            if groups.count == 2 {
                XCTAssertEqual(groups[0].map { $0.node.name }, ["one.bin", "three.bin", "two.bin"])  // most reclaimable space first, oldest first
                XCTAssertEqual(groups[1].map { $0.node.name }, ["small1.txt", "small2.txt"])
            }
            XCTAssertEqual(progress.snapshot.stage, 2)
        }
        let stopped = DuplicateFinder.Cancel()
        stopped.cancel()
        XCTAssertTrue(DuplicateFinder.find(files, cancel: stopped).isEmpty)
    }
}
