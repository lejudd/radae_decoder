#include "audio_input.h"

#include <cmath>
#include <vector>

/* ── construction / destruction ──────────────────────────────────────────── */

AudioInput::AudioInput()  = default;
AudioInput::~AudioInput() { stop(); close(); }

/* ── device enumeration ──────────────────────────────────────────────────── */

std::vector<AudioDevice> AudioInput::enumerate_devices()
{
    return audio_enumerate_capture_devices();
}

std::vector<AudioDevice> AudioInput::enumerate_playback_devices()
{
    return audio_enumerate_playback_devices();
}

/* ── open / close ───────────────────────────────────────────────────────── */

bool AudioInput::open(const std::string& hw_id)
{
    close();

    /* try stereo first, fall back to mono */
    int ch = 2;
    if (!stream_.open(hw_id, true, ch, 44100, 512)) {
        ch = 1;
        if (!stream_.open(hw_id, true, ch, 44100, 512))
            return false;
    }

    channels_ = ch;
    return true;
}

void AudioInput::close()
{
    stop();
    stream_.close();
    channels_ = 0;
    level_left_  = 0.0f;
    level_right_ = 0.0f;
}

/* ── start / stop ───────────────────────────────────────────────────────── */

void AudioInput::start()
{
    if (!stream_.is_open() || running_) return;
    running_ = true;
    thread_  = std::thread(&AudioInput::capture_loop, this);
}

void AudioInput::stop()
{
    if (!running_) return;
    running_ = false;

    if (thread_.joinable()) thread_.join();

    level_left_  = 0.0f;
    level_right_ = 0.0f;
}

/* ── capture loop (dedicated thread) ────────────────────────────────────── */

void AudioInput::capture_loop()
{
    constexpr int READ_FRAMES = 512;
    std::vector<int16_t> buf(READ_FRAMES * channels_);

    while (running_.load(std::memory_order_relaxed)) {

        AudioError err = stream_.read(buf.data(), READ_FRAMES);
        if (err == AUDIO_ERROR) {
            if (!running_.load(std::memory_order_relaxed)) break;
            continue;
        }

        int n = READ_FRAMES;

        /* ── per-channel RMS ───────────────────────────────────────── */
        double sum_l = 0.0, sum_r = 0.0;

        if (channels_ == 1) {
            for (int i = 0; i < n; ++i) {
                double s = buf[i] / 32768.0;
                sum_l   += s * s;
            }
            sum_r = sum_l;
        } else {
            for (int i = 0; i < n; ++i) {
                double l = buf[i * 2]     / 32768.0;
                double r = buf[i * 2 + 1] / 32768.0;
                sum_l   += l * l;
                sum_r   += r * r;
            }
        }

        level_left_.store(static_cast<float>(std::sqrt(sum_l / n)), std::memory_order_relaxed);
        level_right_.store(static_cast<float>(std::sqrt(sum_r / n)), std::memory_order_relaxed);
    }
}
