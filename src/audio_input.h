#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <alsa/asoundlib.h>

/* ── public types ───────────────────────────────────────────────────────── */

struct AudioDevice {
    std::string name;   // human-readable  e.g. "Built-in Audio — Microphone"
    std::string hw_id;  // ALSA identifier e.g. "hw:0,0"
};

/* ── AudioInput ─────────────────────────────────────────────────────────── */

class AudioInput {
public:
    AudioInput();
    ~AudioInput();

    /* device list ---------------------------------------------------------- */
    static std::vector<AudioDevice> enumerate_devices();

    /* lifecycle ------------------------------------------------------------ */
    bool open(const std::string& hw_id);   // configure + prepare
    void close();                          // stop (if running) + close handle
    void start();                          // launch capture thread
    void stop();                           // join capture thread

    /* queries -------------------------------------------------------------- */
    bool  is_running()        const { return running_.load(std::memory_order_relaxed); }
    int   channels()          const { return channels_; }
    float get_level_left()    const { return level_left_.load(std::memory_order_relaxed); }
    float get_level_right()   const { return level_right_.load(std::memory_order_relaxed); }

private:
    void capture_loop();

    snd_pcm_t*         pcm_         = nullptr;
    int                channels_    = 0;
    std::thread        thread_;
    std::atomic<bool>  running_     {false};
    std::atomic<float> level_left_  {0.0f};
    std::atomic<float> level_right_ {0.0f};
};
