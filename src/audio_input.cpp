#include "audio_input.h"

#include <cmath>
#include <vector>

/* ── construction / destruction ──────────────────────────────────────────── */

AudioInput::AudioInput()  = default;
AudioInput::~AudioInput() { stop(); close(); }

/* ── device enumeration ─────────────────────────────────────────────────── */

std::vector<AudioDevice> AudioInput::enumerate_devices()
{
    std::vector<AudioDevice> devices;

    snd_ctl_card_info_t* card_info = nullptr;
    snd_ctl_card_info_alloca(&card_info);

    snd_pcm_info_t* pcm_info = nullptr;
    snd_pcm_info_alloca(&pcm_info);

    int card = -1;                          // snd_card_next starts from -1
    while (snd_card_next(&card) >= 0 && card >= 0) {

        char ctl_name[32];
        snprintf(ctl_name, sizeof ctl_name, "hw:%d", card);

        snd_ctl_t* ctl = nullptr;
        if (snd_ctl_open(&ctl, ctl_name, 0) < 0) continue;

        if (snd_ctl_card_info(ctl, card_info) < 0) {
            snd_ctl_close(ctl);
            continue;
        }
        const char* card_name = snd_ctl_card_info_get_name(card_info);

        int device = -1;
        while (snd_ctl_pcm_next_device(ctl, &device) >= 0 && device >= 0) {
            snd_pcm_info_set_device   (pcm_info, static_cast<unsigned>(device));
            snd_pcm_info_set_subdevice(pcm_info, 0);
            snd_pcm_info_set_stream   (pcm_info, SND_PCM_STREAM_CAPTURE);

            if (snd_ctl_pcm_info(ctl, pcm_info) < 0)
                continue;                   // not a capture-capable device

            AudioDevice ad;
            ad.name  = std::string(card_name ? card_name : "Unknown")
                     + "  \xe2\x80\x94  "          // em-dash UTF-8
                     + snd_pcm_info_get_name(pcm_info);

            char hw[32];
            snprintf(hw, sizeof hw, "hw:%d,%d", card, device);
            ad.hw_id = hw;

            devices.push_back(std::move(ad));
        }
        snd_ctl_close(ctl);
    }
    return devices;
}

/* ── open / close ───────────────────────────────────────────────────────── */

bool AudioInput::open(const std::string& hw_id)
{
    close();                                // tidy up any previous handle

    if (snd_pcm_open(&pcm_, hw_id.c_str(), SND_PCM_STREAM_CAPTURE, 0) < 0)
        return false;

    /* ── hardware parameters ─────────────────────────────────────────── */
    snd_pcm_hw_params_t* hw = nullptr;
    snd_pcm_hw_params_alloca(&hw);

    if (snd_pcm_hw_params_any(pcm_, hw) < 0) goto fail;

    if (snd_pcm_hw_params_set_access(pcm_, hw, SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
        goto fail;
    if (snd_pcm_hw_params_set_format(pcm_, hw, SND_PCM_FORMAT_S16_LE) < 0)
        goto fail;

    {   /* channels: prefer stereo, fall back to mono */
        unsigned int max_ch = 0;
        snd_pcm_hw_params_get_channels_max(hw, &max_ch);
        channels_ = (max_ch >= 2) ? 2 : 1;
        if (snd_pcm_hw_params_set_channels(pcm_, hw, static_cast<unsigned>(channels_)) < 0)
            goto fail;
    }

    {   /* sample-rate: 44100 Hz */
        unsigned int rate = 44100;
        if (snd_pcm_hw_params_set_rate_near(pcm_, hw, &rate, nullptr) < 0)
            goto fail;
    }

    {   /* period / buffer sizes — target ~11 ms period for responsive metering */
        snd_pcm_uframes_t period = 512;
        snd_pcm_uframes_t buffer = 2048;
        snd_pcm_hw_params_set_period_size_near(pcm_, hw, &period, nullptr);
        snd_pcm_hw_params_set_buffer_size_near(pcm_, hw, &buffer);
    }

    if (snd_pcm_hw_params(pcm_, hw) < 0) goto fail;
    if (snd_pcm_prepare(pcm_)        < 0) goto fail;

    return true;

fail:
    snd_pcm_close(pcm_);
    pcm_      = nullptr;
    channels_ = 0;
    return false;
}

void AudioInput::close()
{
    stop();
    if (pcm_) {
        snd_pcm_close(pcm_);
        pcm_      = nullptr;
        channels_ = 0;
    }
    level_left_  = 0.0f;
    level_right_ = 0.0f;
}

/* ── start / stop ───────────────────────────────────────────────────────── */

void AudioInput::start()
{
    if (!pcm_ || running_) return;
    running_ = true;
    thread_  = std::thread(&AudioInput::capture_loop, this);
}

void AudioInput::stop()
{
    if (!running_) return;
    running_ = false;

    if (pcm_) snd_pcm_drop(pcm_);       // unblock a pending readi()

    if (thread_.joinable()) thread_.join();

    level_left_  = 0.0f;
    level_right_ = 0.0f;
}

/* ── capture loop (dedicated thread) ────────────────────────────────────── */

void AudioInput::capture_loop()
{
    constexpr snd_pcm_uframes_t READ_FRAMES = 512;
    std::vector<int16_t> buf(READ_FRAMES * static_cast<unsigned>(channels_));

    while (running_.load(std::memory_order_relaxed)) {

        snd_pcm_sframes_t n = snd_pcm_readi(pcm_, buf.data(), READ_FRAMES);

        /* ── error handling ────────────────────────────────────────── */
        if (n < 0) {
            if (n == -EINTR) continue;                      // signal — retry
            if (!running_.load(std::memory_order_relaxed))  // shutting down
                break;
            n = snd_pcm_recover(pcm_, static_cast<int>(n), 0);
            if (n < 0) break;                               // unrecoverable
            continue;
        }
        if (n == 0) continue;

        /* ── per-channel RMS ───────────────────────────────────────── */
        double sum_l = 0.0, sum_r = 0.0;

        if (channels_ == 1) {
            for (snd_pcm_sframes_t i = 0; i < n; ++i) {
                double s = buf[i] / 32768.0;
                sum_l   += s * s;
            }
            sum_r = sum_l;                  // duplicate mono → both channels
        } else {
            for (snd_pcm_sframes_t i = 0; i < n; ++i) {
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
