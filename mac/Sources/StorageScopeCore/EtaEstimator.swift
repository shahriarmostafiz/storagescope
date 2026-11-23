import Foundation

/// Percent and time left of a scan. Feed it the clock, how much is done and how much is expected in total (any unit: files, bytes).
/// Time left is the remaining work over the scan speed, and the number on screen
///   - falls no faster than the clock (a countdown must not drop from 5 minutes to 20 seconds), and
///   - rises toward a new estimate over about 10 seconds, so a slow stretch is shown, but not as a jump.
/// `cautious` (a first scan, where the total is only a guess) takes the slower of the average and the recent speed; otherwise the mean.
public struct EtaEstimator {
    public struct Result {
        public var frac = 0.0      // 0 ... 0.99
        public var ready = false   // false while there is too little to go on (the first seconds, or nearly done)
        public var seconds = 0.0   // time left, only when ready
    }

    private var cautious: Bool
    private var lastTime = 0.0, shownTime = 0.0, lastDone = 0.0, rate = 0.0, shown = 0.0

    public init(cautious: Bool = false) { self.cautious = cautious }

    public mutating func update(elapsed: Double, done: Double, total: Double) -> Result {
        var r = Result()
        r.frac = total > 0 ? min(0.99, done / total) : 0
        if elapsed >= lastTime + 1 {  // speed over the last stretch, smoothed (it differs a lot between big and small folders)
            let inst = max(0, done - lastDone) / (elapsed - lastTime)
            rate = rate <= 0 ? inst : rate * 0.8 + inst * 0.2
            lastTime = elapsed
            lastDone = done
        }
        let avg = elapsed > 0 ? done / elapsed : 0
        let speed = cautious ? min(avg, rate > 0 ? rate : avg) : (avg + rate) / 2
        if elapsed < 6 || speed <= 0 || r.frac < 0.01 || r.frac >= 0.97 { return r }
        var est = max(0, total - done) / speed
        if shown > 0 {
            let dt = elapsed - shownTime
            est = est < shown ? max(est, shown - dt) : shown + (est - shown) * (1 - exp(-dt / 10))
        }
        shown = est
        shownTime = elapsed
        r.ready = true
        r.seconds = est
        return r
    }
}
