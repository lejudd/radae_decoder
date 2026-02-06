#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <alsa/asoundlib.h>

/* Forward declaration — avoids exposing RADE/FARGAN C headers in this header */
struct rade;

/* ── RadaeDecoder ──────────────────────────────────────────────────────────
 *
 *  Real-time RADAE decoder pipeline:
 *    ALSA capture → resample → Hilbert → RADE Rx → FARGAN → resample → ALSA playback
 *
 *  All processing runs on a dedicated thread.  Status is exposed via atomics.
 * ──────────────────────────────────────────────────────────────────────── */

class RadaeDecoder {
public:
    RadaeDecoder();
    ~RadaeDecoder();

    /* lifecycle -------------------------------------------------------------- */
    bool open(const std::string& input_hw_id, const std::string& output_hw_id);
    void close();
    void start();
    void stop();

    /* status queries (thread-safe) ------------------------------------------ */
    bool  is_running()            const { return running_.load(std::memory_order_relaxed); }
    bool  is_synced()             const { return synced_.load(std::memory_order_relaxed); }
    float snr_dB()                const { return snr_dB_.load(std::memory_order_relaxed); }
    float freq_offset()           const { return freq_offset_.load(std::memory_order_relaxed); }
    float get_output_level_left() const { return output_level_.load(std::memory_order_relaxed); }
    float get_output_level_right()const { return output_level_.load(std::memory_order_relaxed); } // mono

private:
    void processing_loop();

    /* ── ALSA handles ─────────────────────────────────────────────────────── */
    snd_pcm_t*   pcm_in_   = nullptr;
    snd_pcm_t*   pcm_out_  = nullptr;
    unsigned int  rate_in_  = 0;   // negotiated capture rate
    unsigned int  rate_out_ = 0;   // negotiated playback rate

    /* ── RADE receiver (opaque) ───────────────────────────────────────────── */
    struct rade*  rade_     = nullptr;

    /* ── FARGAN vocoder (opaque void* to avoid C header in .h) ────────────── */
    void*         fargan_   = nullptr;

    /* ── Hilbert transform (127-tap FIR) ──────────────────────────────────── */
    static constexpr int HILBERT_NTAPS = 127;
    static constexpr int HILBERT_DELAY = (HILBERT_NTAPS - 1) / 2;  /* 63 */
    float hilbert_coeffs_[HILBERT_NTAPS] = {};
    float hilbert_hist_[HILBERT_NTAPS]   = {};   // history for FIR
    int   hilbert_pos_                   = 0;    // write position in history

    /* ── FARGAN warmup state ──────────────────────────────────────────────── */
    static constexpr int NB_TOTAL_FEAT = 36;
    bool  fargan_ready_    = false;
    int   warmup_count_    = 0;
    float warmup_buf_[5 * 36] = {};   // 5 frames × NB_TOTAL_FEATURES

    /* ── Resampler state (input) ──────────────────────────────────────────── */
    double resamp_in_frac_ = 0.0;   // fractional sample position for input resampler
    float  resamp_in_prev_ = 0.0f;  // previous input sample for linear interpolation

    /* ── Delay buffer for Hilbert real part ────────────────────────────────── */
    float delay_buf_[HILBERT_NTAPS] = {};
    int   delay_pos_                = 0;

    /* ── Thread & atomics ─────────────────────────────────────────────────── */
    std::thread        thread_;
    std::atomic<bool>  running_     {false};
    std::atomic<bool>  synced_      {false};
    std::atomic<float> snr_dB_      {0.0f};
    std::atomic<float> freq_offset_ {0.0f};
    std::atomic<float> output_level_{0.0f};
};
