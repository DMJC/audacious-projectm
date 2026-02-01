#pragma once
#include <atomic>
#include <vector>
#include <algorithm>

class StereoFloatRing {
public:
    explicit StereoFloatRing(size_t frames_capacity)
        : buf_(frames_capacity * 2), cap_(frames_capacity * 2) {}

    void push(const float* interleaved_stereo, size_t frames) {
        const size_t count = frames * 2;
        size_t w = w_.load(std::memory_order_relaxed);
        size_t r = r_.load(std::memory_order_acquire);

        size_t free = (r + cap_ - w - 1) % cap_;
        size_t n = std::min(count, free);

        for (size_t i = 0; i < n; ++i)
            buf_[(w + i) % cap_] = interleaved_stereo[i];

        w_.store((w + n) % cap_, std::memory_order_release);
    }

    // returns frames popped
    size_t pop(float* out_interleaved_stereo, size_t max_frames) {
        const size_t max_count = max_frames * 2;
        size_t r = r_.load(std::memory_order_relaxed);
        size_t w = w_.load(std::memory_order_acquire);

        size_t avail = (w + cap_ - r) % cap_;
        size_t n = std::min(max_count, avail);
        n -= (n % 2); // keep stereo alignment

        for (size_t i = 0; i < n; ++i)
            out_interleaved_stereo[i] = buf_[(r + i) % cap_];

        r_.store((r + n) % cap_, std::memory_order_release);
        return n / 2;
    }

    void clear() {
        r_.store(0, std::memory_order_relaxed);
        w_.store(0, std::memory_order_relaxed);
    }

private:
    std::vector<float> buf_;
    const size_t cap_;
    std::atomic<size_t> r_{0}, w_{0};
};
