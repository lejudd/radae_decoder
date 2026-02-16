#include "audio_input.h"

#include <cmath>
#include <vector>
#include <pulse/pulseaudio.h>

/* ── construction / destruction ──────────────────────────────────────────── */

AudioInput::AudioInput()  = default;
AudioInput::~AudioInput() { stop(); close(); }

/* ── device enumeration via PulseAudio introspection API ─────────────────── */

struct EnumCtx {
    std::vector<AudioDevice>* devices;
    bool done;
};

static void source_info_cb(pa_context* /*c*/, const pa_source_info* i,
                            int eol, void* userdata)
{
    auto* ctx = static_cast<EnumCtx*>(userdata);
    if (eol > 0) { ctx->done = true; return; }
    if (!i) return;
    /* skip monitors (playback device loopbacks) */
    if (i->monitor_of_sink != PA_INVALID_INDEX) return;

    AudioDevice ad;
    ad.name  = i->description ? i->description : i->name;
    ad.hw_id = i->name;
    ctx->devices->push_back(std::move(ad));
}

static void sink_info_cb(pa_context* /*c*/, const pa_sink_info* i,
                          int eol, void* userdata)
{
    auto* ctx = static_cast<EnumCtx*>(userdata);
    if (eol > 0) { ctx->done = true; return; }
    if (!i) return;

    AudioDevice ad;
    ad.name  = i->description ? i->description : i->name;
    ad.hw_id = i->name;
    ctx->devices->push_back(std::move(ad));
}

static void context_state_cb(pa_context* c, void* userdata)
{
    auto* ready = static_cast<bool*>(userdata);
    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_READY:
        case PA_CONTEXT_FAILED:
        case PA_CONTEXT_TERMINATED:
            *ready = true;
            break;
        default:
            break;
    }
}

static std::vector<AudioDevice> enumerate_pa_devices(bool capture)
{
    std::vector<AudioDevice> devices;

    pa_mainloop* ml = pa_mainloop_new();
    if (!ml) return devices;

    pa_context* ctx = pa_context_new(pa_mainloop_get_api(ml), "radae-enum");
    if (!ctx) { pa_mainloop_free(ml); return devices; }

    bool ready = false;
    pa_context_set_state_callback(ctx, context_state_cb, &ready);
    pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr);

    /* wait for connection */
    while (!ready)
        pa_mainloop_iterate(ml, 1, nullptr);

    if (pa_context_get_state(ctx) != PA_CONTEXT_READY) {
        pa_context_unref(ctx);
        pa_mainloop_free(ml);
        return devices;
    }

    EnumCtx ectx{&devices, false};
    pa_operation* op;
    if (capture)
        op = pa_context_get_source_info_list(ctx, source_info_cb, &ectx);
    else
        op = pa_context_get_sink_info_list(ctx, sink_info_cb, &ectx);

    while (!ectx.done)
        pa_mainloop_iterate(ml, 1, nullptr);

    if (op) pa_operation_unref(op);
    pa_context_disconnect(ctx);
    pa_context_unref(ctx);
    pa_mainloop_free(ml);

    return devices;
}

std::vector<AudioDevice> AudioInput::enumerate_devices()
{
    return enumerate_pa_devices(true);
}

std::vector<AudioDevice> AudioInput::enumerate_playback_devices()
{
    return enumerate_pa_devices(false);
}

/* ── open / close ───────────────────────────────────────────────────────── */

bool AudioInput::open(const std::string& hw_id)
{
    close();

    pa_sample_spec ss{};
    ss.format   = PA_SAMPLE_S16LE;
    ss.rate     = 44100;
    ss.channels = 2;

    int error = 0;
    pa_ = pa_simple_new(nullptr, "RADAE Decoder", PA_STREAM_RECORD,
                        hw_id.c_str(), "level-meter",
                        &ss, nullptr, nullptr, &error);
    if (!pa_) {
        /* fall back to mono */
        ss.channels = 1;
        pa_ = pa_simple_new(nullptr, "RADAE Decoder", PA_STREAM_RECORD,
                            hw_id.c_str(), "level-meter",
                            &ss, nullptr, nullptr, &error);
    }
    if (!pa_) return false;

    channels_ = ss.channels;
    return true;
}

void AudioInput::close()
{
    stop();
    if (pa_) {
        pa_simple_free(pa_);
        pa_       = nullptr;
        channels_ = 0;
    }
    level_left_  = 0.0f;
    level_right_ = 0.0f;
}

/* ── start / stop ───────────────────────────────────────────────────────── */

void AudioInput::start()
{
    if (!pa_ || running_) return;
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

        int error = 0;
        int ret = pa_simple_read(pa_, buf.data(),
                                 buf.size() * sizeof(int16_t), &error);
        if (ret < 0) {
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
