// braid_timer.cpp — Timer (ofTimerFps port): lazy sleep until ~3ms before the
// target wake, then a tight yield-spin for the final stretch. No GPU, no
// windowing — a pure CPU utility, isolated so it can be unit-tested headless.
#include "braid.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace braid {

Timer::Timer(int targetFps) : targetFps_(targetFps) {
    interval_ = nanos(1'000'000'000LL / std::max(1, targetFps));
    reset();
}

void Timer::setFps(int fps) {
    targetFps_ = std::max(1, fps);
    interval_ = nanos(1'000'000'000LL / targetFps_);
}

void Timer::reset() {
    startTime_ = clock::now();
    wakeTime_ = startTime_ + interval_;
    lastWakeTime_ = startTime_;
    frames_ = 0;
    delta_ = 0.0f;
    elapsed_ = 0.0f;
}

void Timer::waitNext() {
    // Uncapped: targetFps <= 0 means run free — no pacing, just measure.
    if (targetFps_ <= 0) {
        const auto now = clock::now();
        delta_ = std::chrono::duration<float>(now - lastWakeTime_).count();
        elapsed_ = std::chrono::duration<float>(now - startTime_).count();
        lastWakeTime_ = now;
        ++frames_;
        return;
    }
    // 1) Lazy sleep until ~3ms before the target wake time.
    const auto slack = std::chrono::milliseconds(3);
    if (wakeTime_ - slack > clock::now()) {
        std::this_thread::sleep_until(wakeTime_ - slack);
    }
    // 2) Tight yield spin for the final stretch.
    while (clock::now() < wakeTime_) {
        std::this_thread::yield();
    }
    // 3) Bookkeeping + advance the target.
    const auto now = clock::now();
    delta_ = std::chrono::duration<float>(now - lastWakeTime_).count();
    elapsed_ = std::chrono::duration<float>(now - startTime_).count();
    lastWakeTime_ = now;
    wakeTime_ += interval_;
    // If we fell badly behind, don't spiral — resync the target.
    if (wakeTime_ < now) wakeTime_ = now + interval_;
    ++frames_;
}

int Timer::currentFps() const {
    return delta_ > 0.0f ? static_cast<int>(std::lround(1.0f / delta_)) : targetFps_;
}

}  // namespace braid
