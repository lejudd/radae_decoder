# Technical notes

## RADAE Encoder

### Stage-by-Stage Breakdown

#### 1. Audio Capture (PulseAudio, 16 kHz)

- Captures S16LE mono audio from the microphone via PulseAudio
- Applies a user-configurable **mic gain** for input level control
- If the capture device isn't natively 16 kHz, a streaming linear-interpolation resampler converts to 16 kHz (the rate LPCNet expects)

#### 2. LPCNet Feature Extraction

- Processes audio in **10 ms frames** (160 samples at 16 kHz)
- For each frame, `lpcnet_compute_single_frame_features()` extracts a vector of `NB_TOTAL_FEATURES` (36) values:
  - **18 Bark-scale cepstral coefficients** — compact spectral envelope representation
  - **Pitch period and pitch correlation** — fundamental frequency information
  - **Other auxiliary features** used by the downstream neural encoder
- **12 consecutive feature frames** (120 ms of speech) are accumulated into a single modem frame buffer of 432 features (12 × 36)

#### 3. RADE Neural Encoder (`rade_tx`)

- Once a full modem frame (432 features) is accumulated, it is passed to `rade_tx()`
- The RADE encoder is a **neural network (autoencoder)** that compresses the 432-dimensional feature vector into a sequence of **OFDM-like complex symbols** (960 IQ samples at 8 kHz)
- The encoding is designed to be **robust to HF radio channel impairments** — multipath fading, frequency offset, noise — by learning a channel-resilient latent representation during training
- The complex output represents a baseband waveform ready for transmission

#### 4. TX Bandpass Filter (Optional)

- An optional bandpass filter (centre 1600 Hz, bandwidth 1500 Hz) constrains the output spectrum to the **SSB passband** (~700–2300 Hz)
- Prevents out-of-band energy from causing interference on adjacent frequencies
- Can be toggled at runtime via `set_bpf_enabled()`

#### 5. Output to Radio (PulseAudio, 8 kHz)

- The real part of the complex IQ signal is extracted and scaled by a configurable **TX scale** factor (default 16384)
- Resampled from the 8 kHz modem rate to the output device's native rate if needed
- Written to the radio soundcard via PulseAudio for transmission as an SSB audio signal

#### 6. End-of-Over (`rade_tx_eoo`)

- When transmission stops, a special **end-of-over** frame (1152 samples) is sent via `rade_tx_eoo()`
- This signals to the receiver that the transmission is complete, allowing clean decoder shutdown

### Additional Features

- **Input/output level metering**: RMS levels are computed and exposed via thread-safe atomics for UI display
- **TX spectrum display**: A 512-point FFT with a Hann window computes the power spectrum of the TX output, available via `get_spectrum()` for real-time visualisation
- **Prefill buffer**: 2 modem frames of silence are written to the output buffer at startup to absorb the ~120 ms latency of accumulating 12 feature frames before the first output is produced

### Timing

| Parameter | Value |
|---|---|
| Feature frame | 10 ms (160 samples @ 16 kHz) |
| Modem frame | 120 ms (12 feature frames) |
| TX output per modem frame | 960 IQ samples @ 8 kHz |
| End-of-over frame | 1152 IQ samples @ 8 kHz |
| Algorithmic latency | ~120 ms (accumulation) + audio buffering |

### Reference

- RADE is part of the **Codec 2** project by David Rowe (VK5DGR)
- Built on top of the Opus/LPCNet neural speech processing framework

## RADAE Decoder

### Stage-by-Stage Breakdown

#### 1. Audio Capture (PulseAudio, 16 kHz)

- Captures S16LE mono audio from the microphone via PulseAudio
- Applies a user-configurable **mic gain** for input level control
- If the capture device isn't natively 16 kHz, a streaming linear-interpolation resampler converts to 16 kHz (the rate LPCNet expects)

#### 2. LPCNet Feature Extraction

- Processes audio in **10 ms frames** (160 samples at 16 kHz)
- For each frame, `lpcnet_compute_single_frame_features()` extracts a vector of `NB_TOTAL_FEATURES` (36) values:
  - **18 Bark-scale cepstral coefficients** — compact spectral envelope representation
  - **Pitch period and pitch correlation** — fundamental frequency information
  - **Other auxiliary features** used by the downstream neural encoder
- **12 consecutive feature frames** (120 ms of speech) are accumulated into a single modem frame buffer of 432 features (12 × 36)

#### 3. RADE Neural Encoder (`rade_tx`)

- Once a full modem frame (432 features) is accumulated, it is passed to `rade_tx()`
- The RADE encoder is a **neural network (autoencoder)** that compresses the 432-dimensional feature vector into a sequence of **OFDM-like complex symbols** (960 IQ samples at 8 kHz)
- The encoding is designed to be **robust to HF radio channel impairments** — multipath fading, frequency offset, noise — by learning a channel-resilient latent representation during training
- The complex output represents a baseband waveform ready for transmission

#### 4. TX Bandpass Filter (Optional)

- An optional bandpass filter (centre 1600 Hz, bandwidth 1500 Hz) constrains the output spectrum to the **SSB passband** (~700–2300 Hz)
- Prevents out-of-band energy from causing interference on adjacent frequencies
- Can be toggled at runtime via `set_bpf_enabled()`

#### 5. Output to Radio (PulseAudio, 8 kHz)

- The real part of the complex IQ signal is extracted and scaled by a configurable **TX scale** factor (default 16384)
- Resampled from the 8 kHz modem rate to the output device's native rate if needed
- Written to the radio soundcard via PulseAudio for transmission as an SSB audio signal

#### 6. End-of-Over (`rade_tx_eoo`)

- When transmission stops, a special **end-of-over** frame (1152 samples) is sent via `rade_tx_eoo()`
- This signals to the receiver that the transmission is complete, allowing clean decoder shutdown

### Additional Features

- **Input/output level metering**: RMS levels are computed and exposed via thread-safe atomics for UI display
- **TX spectrum display**: A 512-point FFT with a Hann window computes the power spectrum of the TX output, available via `get_spectrum()` for real-time visualisation
- **Prefill buffer**: 2 modem frames of silence are written to the output buffer at startup to absorb the ~120 ms latency of accumulating 12 feature frames before the first output is produced

### Timing

| Parameter | Value |
|---|---|
| Feature frame | 10 ms (160 samples @ 16 kHz) |
| Modem frame | 120 ms (12 feature frames) |
| TX output per modem frame | 960 IQ samples @ 8 kHz |
| End-of-over frame | 1152 IQ samples @ 8 kHz |
| Algorithmic latency | ~120 ms (accumulation) + audio buffering |

### Reference

- RADE is part of the **Codec 2** project by David Rowe (VK5DGR)
- Built on top of the Opus/LPCNet neural speech processing framework

## LPCNet

LPCNet is a neural network-based speech synthesis model developed by Jean-Marc Valin (Mozilla/Xiph.org), designed for **low-complexity, real-time speech coding**.

### Core Idea

It combines two classic techniques:

1. **Linear Predictive Coding (LPC)** — a traditional DSP method that models the vocal tract as an all-pole filter. LPC captures the spectral envelope (formants) of speech very efficiently.

2. **WaveRNN** — a recurrent neural network that generates audio samples one at a time.

The key insight is: instead of having the neural network learn *everything* about the speech signal, let the LPC filter handle the predictable spectral structure, and have the neural network only model the **residual excitation signal**. This dramatically reduces the complexity of what the network needs to learn.

### Architecture

- **Frame-rate network**: Processes features (Bark-scale cepstral coefficients, pitch period, pitch correlation) once per frame (~10ms), producing a conditioning vector.
- **Sample-rate network**: A lightweight GRU that generates one audio sample at a time, conditioned on the frame-rate output and the LPC prediction. It outputs a probability distribution over sample values, using a dual-softmax formulation.

### Why It Matters

- Runs in **real-time on a single CPU core** (no GPU needed)
- Achieves **near-transparent speech quality** at very low bitrates (~1.6 kbps when paired with a codec like Codec 2)
- The LPC "shortcut" reduces the neural network's workload by ~30dB, making the tiny network feasible

### Relevance to This Project

This project uses LPCNet (or a derivative like RADE — Radio Autoencoder) for encoding/decoding speech over radio channels, where robustness to channel errors and low bitrate are critical.

### Reference

- Original paper: *"LPCNet: Improving Neural Speech Synthesis Through Linear Prediction"* (Valin & Skoglund, 2019, ICASSP)


## FARGAN (Framewise Auto-Regressive GAN)

FARGAN is a neural vocoder developed by Jean-Marc Valin as a successor to LPCNet, designed for **ultra-low complexity speech synthesis** while maintaining high audio quality.

### Motivation

LPCNet generates audio **one sample at a time**, which creates a sequential bottleneck. Even though it's efficient for a neural vocoder, sample-level autoregression limits parallelism and throughput. FARGAN addresses this by operating at the **frame level** instead.

### Core Idea

Rather than predicting individual samples, FARGAN generates an entire frame of audio (~10ms / 160 samples at 16kHz) in one step, using a **GAN (Generative Adversarial Network)** training framework. It retains the LPC filtering concept from LPCNet to reduce what the network must learn.

### Architecture

- **Frame-level autoregression**: The model is autoregressive across *frames*, not *samples*. Each forward pass produces a full frame of audio, conditioned on previous frames.
- **LPC synthesis filter**: Like LPCNet, an LPC filter models the vocal tract. The neural network generates the excitation signal, which is then passed through the LPC filter to produce the final waveform.
- **Sub-frame structure**: Each frame is broken into sub-frames, with a lightweight recurrent structure operating across sub-frames for temporal coherence.
- **GAN training**: A discriminator network is used during training to push the generator toward producing natural-sounding speech (adversarial loss), combined with feature matching and spectral losses.

### Key Advantages Over LPCNet

| Feature | LPCNet | FARGAN |
|---|---|---|
| Generation unit | 1 sample | 1 frame (~160 samples) |
| Autoregression | Sample-level | Frame-level |
| Training | Cross-entropy | GAN (adversarial) |
| Complexity | ~3 GFLOPS | ~0.5 GFLOPS |
| Parallelism | Minimal (sequential samples) | High (within-frame parallel) |

### Why It Matters

- **~6x less compute** than LPCNet while achieving comparable or better quality
- Easily runs in **real-time on low-power devices** (embedded, mobile)
- Well-suited for pairing with low-bitrate speech codecs (e.g., Codec 2, RADE) where the vocoder reconstructs speech from compact feature vectors
- GAN training produces crisper, more natural output than the probability-based sample generation in LPCNet

### Relevance to RADE / This Project

In the RADE (Radio Autoencoder) pipeline, FARGAN serves as the **decoder-side vocoder** — it takes the decoded feature vectors (Bark-scale cepstral coefficients, pitch parameters) received over the radio channel and synthesizes them back into audible speech. Its low complexity makes it practical for real-time amateur radio applications.

### Reference

- *"FARGAN: Efficient Neural Audio Synthesis with Framewise Auto-Regressive GAN"* (Valin, 2024)
- Built on the Opus/Codec 2 ecosystem from Xiph.org

