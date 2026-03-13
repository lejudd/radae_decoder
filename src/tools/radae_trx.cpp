/*---------------------------------------------------------------------------*\

  radae_trx.cpp

  RADAE headless mode transceiver - experimental
  Work in progress.
  Runtime-switchable (RX <-> TX) version of radae_headless
  Keyboard: space = toggle, t = to TX, r = to RX, q = quit
  Re-uses config loading, audio devices, callsign from radae_headless logic

\*---------------------------------------------------------------------------*/

/*
  Copyright (C) 2024 David Rowe

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions
  are met:

  - Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  - Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <fstream>
#include <sstream>
#include <getopt.h>
#include <signal.h>

#include "../src/radae_top/rade_decoder.h"
#include "../src/radae_top/rade_encoder.h"
#include "../src/audio/audio_stream.h"

/* ── Globals ──────────────────────────────────────────────────────────── */

// 
std::atomic<bool> g_quit{false};
std::atomic<bool> g_is_tx{false};               // true = TX mode
std::atomic<bool> g_switch_requested{false};

/* ── Configuration structure ──────────────────────────────────────────── */

struct Config {
    std::string fromradio;
    std::string toradio;
    std::string frommic;
    std::string tospeaker;
    std::string call;
    float tx_level = 1.0f;
    bool bpf = true;
};

Config config;

void signal_handler(int signum) {
    (void)signum;
    g_quit = true;
};

std::thread g_worker_thread;

class TerminalRawMode {
public:
    TerminalRawMode() {
        tcgetattr(STDIN_FILENO, &orig_);
        raw_ = orig_;
        raw_.c_lflag &= ~(ICANON | ECHO | ISIG);
        raw_.c_cc[VMIN] = 0;
        raw_.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw_);
    }
    ~TerminalRawMode() {
        tcsetattr(STDIN_FILENO, TCSANOW, &orig_);
        std::cout << std::endl;
    }
private:
    struct termios orig_{}, raw_{};
};

bool load_config(const std::string& path = "radae_headless.conf") {
    std::ifstream f(path);
    if (!f) {
        std::cerr << "No config: " << path << " - using defaults\n";
        return false;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq+1);
        if (key == "fromradio")    config.fromradio = value;
        else if (key == "tospeaker") config.tospeaker = value;
        else if (key == "frommic")   config.frommic = value;
        else if (key == "toradio")   config.toradio = value;
        else if (key == "call")  config.call = value;
        else if (key == "tx_output_level") config.tx_level = std::stof(value);
        else if (key == "bpf_enable") config.bpf = (value == "true" || value == "1");
    }
    return true;
}

void stop_current() {
    g_quit = true;
    if (g_worker_thread.joinable()) {
        g_worker_thread.join();
    }
    g_quit = false;
}

bool start_worker() {
    std::cout << "Starting " << (g_is_tx ? "TX" : "RX") << "...\n";

    g_worker_thread = std::thread([]() {
        if (g_is_tx) {
            RadaeEncoder encoder;

            encoder.set_bpf_enabled(config.bpf);
            encoder.set_callsign(config.call);

            if (!encoder.open(config.frommic, config.toradio)) {
                fprintf(stderr, "Error: Failed to open encoder devices\n");
                g_quit = true;
            }

            encoder.start();

            while (!g_quit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                float input_level = encoder.get_input_level();
                float output_level = encoder.get_output_level();
                fprintf(stderr, "\rInput: %.2f  Output: %.2f  ", input_level, output_level);
                fflush(stderr);
            }

            encoder.stop();
            encoder.close();

        } else {
            RadaeDecoder decoder;

            if (!decoder.open(config.fromradio, config.tospeaker)) {
                fprintf(stderr, "Error: Failed to open decoder devices\n");
                g_quit = true;
            }

            decoder.start();

            while (!g_quit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                /* Print status */
                bool synced = decoder.is_synced();
                float snr = decoder.snr_dB();
                float freq_offset = decoder.freq_offset();
                float input_level = decoder.get_input_level();
                float output_level = decoder.get_output_level_left();
                std::string last_callsign = decoder.last_callsign();

                fprintf(stderr, "\r%s SNR: %.1f dB  Freq: %+.1f Hz  In: %.2f  Out: %.2f  Last Call: %s ",
                        synced ? "SYNC" : "----", snr, freq_offset, input_level, output_level, last_callsign.c_str());
                fflush(stderr);
            }

            decoder.stop();
            decoder.close();
        }
    });

    return true;
}

void request_switch(bool want_tx) {
    if (want_tx == g_is_tx) return;

    std::cout << "\nSwitch to " << (want_tx ? "TX" : "RX") << "...\n";

    stop_current();

    g_is_tx = want_tx;
    g_switch_requested = false;

    if (!start_worker()) {
        std::cerr << "Error: Start failed - quitting\n";
        g_quit = true;
    }
}

void keyboard_thread() {
    pollfd pfd{STDIN_FILENO, POLLIN, 0};
    while (!g_quit) {
        if (poll(&pfd, 1, 60) <= 0) continue;

        char buf[16];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) continue;

        for (ssize_t i = 0; i < n; ++i) {
            char c = buf[i];
            bool handled = false;

            if (c == ' ' ) {
                request_switch(!g_is_tx);
                handled = true;
            }
            else if (c == 't' || c == 'T') {
                request_switch(true);
                handled = true;
            }
            else if (c == 'r' || c == 'R') {
                request_switch(false);
                handled = true;
            }
            else if (c == 'q' || c == 'Q') {
                std::cout << "\nQuit.\n";
                g_quit = true;
                return;
            }

            if (handled) {
                // Optional: consume extra bytes from esc sequences
            }
        }
    }
}

void status_thread() {
    while (!g_quit) {
        std::cout << "\r\033[K" << (g_is_tx ? "[TX]" : "[RX]")
        << " | Call: " << config.call
        << std::flush;
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
}

int main(int argc, char** argv) {
    std::cout << "RADAE TRX - switchable transceiver\n";
    std::cout << " space → toggle RX/TX   t → TX   r → RX   q → quit\n\n";

    load_config();

    TerminalRawMode raw_mode;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    audio_init();
    rade_initialize();

    if (!start_worker()) {
        return 1;
    }

    std::thread kb(keyboard_thread);

    while (!g_quit) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (g_switch_requested) {
            request_switch(!g_is_tx);
        }
    }

    kb.join();

    stop_current();

    rade_finalize();
    audio_terminate();

    std::cout << "\nDone.\n";
    return 0;
}
