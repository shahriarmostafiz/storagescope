#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ss {

struct EtaResult {
    double frac = 0;      // 0..0.99
    bool ready = false;   // false while there is too little to go on (the first seconds, or nearly done)
    double seconds = 0;   // time left, only when ready
};

// Percent and time left of a scan. Feed it the clock, how much is done and how much is expected in total (any unit: entries, bytes).
// Time left is the remaining work over the scan speed, and the number on screen
//   - falls no faster than the clock (a countdown must not drop from 5 minutes to 20 seconds), and
//   - rises toward a new estimate over about 10 seconds, so a slow stretch is shown, but not as a jump.
// `cautious` (a first scan, where the total is only a guess) takes the slower of the average and the recent speed; otherwise the mean.
class EtaEstimator {
public:
    void Reset(bool cautious) { *this = EtaEstimator(); cautious_ = cautious; }

    EtaResult Update(uint64_t ms, double done, double total) {
        EtaResult r;
        r.frac = total > 0 ? std::min(0.99, done / total) : 0;
        if (ms >= lastMs_ + 1000) {  // speed over the last stretch, smoothed (it differs a lot between big and small folders)
            double inst = std::max(0.0, done - lastDone_) * 1000.0 / static_cast<double>(ms - lastMs_);
            rate_ = rate_ <= 0 ? inst : rate_ * 0.8 + inst * 0.2;
            lastMs_ = ms;
            lastDone_ = done;
        }
        double avg = ms ? done * 1000.0 / static_cast<double>(ms) : 0;
        double rate = cautious_ ? std::min(avg, rate_ > 0 ? rate_ : avg) : (avg + rate_) / 2;
        if (ms < 6000 || rate <= 0 || r.frac < 0.01 || r.frac >= 0.97) return r;
        double est = std::max(0.0, total - done) / rate;
        if (shown_ > 0) {
            double dt = static_cast<double>(ms - shownMs_) / 1000.0;
            est = est < shown_ ? std::max(est, shown_ - dt) : shown_ + (est - shown_) * (1 - std::exp(-dt / 10.0));
        }
        shown_ = est;
        shownMs_ = ms;
        r.ready = true;
        r.seconds = est;
        return r;
    }

private:
    bool cautious_ = false;
    uint64_t lastMs_ = 0, shownMs_ = 0;
    double lastDone_ = 0, rate_ = 0, shown_ = 0;
};

}  // namespace ss
