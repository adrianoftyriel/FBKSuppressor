# FBKSuppressor

A zero-latency VST3 / AU / standalone plugin for live feedback and room-noise
suppression, running natively at 44.1 kHz and 48 kHz.

It removes PA feedback, broadband room noise and mains hum **subtractively** —
by working out what the interference is and cancelling that specific waveform —
rather than by attenuating the frequencies the interference happens to occupy.
The distinction is the entire point of the project: a notch at 2.1 kHz removes
your voice at 2.1 kHz too, and it keeps removing it. Cancelling a 2.1 kHz
sinusoid removes only that sinusoid.

There is no ringing out, no static EQ, no notch filters, no gates, no thresholds.

---

## Status

**v0.1 — signal processing only.** Everything here is deterministic DSP with no
trained parameters, which means it behaves predictably on material no model has
ever seen. The learned voice/non-voice separator is v0.2; the interface for it is
already in the code (`SpectralSeparator`), and adding it will be a substitution
rather than a rewrite.

---

## What "zero latency" means here, precisely

The plugin reports **0 samples** of latency to the host and adds **0 samples** of
delay to the audio. Verified by test, at both sample rates, at a 16-sample buffer:
feed an impulse in and the very first output sample already responds.

That is not the same as claiming the impossible. Nothing can remove a sound in
the same instant it arrives without having seen it first. What the plugin does is
analyse **only the past** and never delay the output:

- The analysis STFT uses a long trailing window, so it has real frequency
  resolution — but it is analysis-only. There is no overlap-add resynthesis
  anywhere, and overlap-add is precisely what forces a window-length delay. This
  is where RNNoise's ~10 ms and DeepFilterNet's 40 ms come from.
- The resulting spectral mask is applied as a **minimum-phase FIR**. Of every
  filter with a given magnitude response, the minimum-phase one has the smallest
  possible group delay; its impulse response is causal with its energy in the
  first few taps. Measured energy centroid: **0.00 samples**.
- Feedback and hum are **predicted and subtracted** in the time domain.

The practical consequence, stated plainly:

| Interference | Predictable from the past? | Removed at zero latency? |
|---|---|---|
| PA feedback / howling | Yes — a slowly growing sinusoid | Yes, ~75 dB |
| Mains hum and buzz | Yes — exactly periodic | Yes, ~85 dB |
| Steady room noise (HVAC, hiss, rumble) | Largely | Yes, up to the attenuation limit |
| A sharp unpredictable transient (chair scrape, mic thump) | No | **No** |

For the stated goal — room noise and PA feedback, voice character untouched —
that trade falls on the right side. A **Quality mode** is provided for anyone who
would rather have a linear-phase filter and pay 128 samples (2.7 ms at 48 kHz) for
it; it is off by default and the host compensates it when on.

---

## Measured performance

From the test suite (`./build/tests/fbk_tests`, 54 checks, all passing under
Linux GCC, Linux Clang, MSVC and Apple Clang):

| Measurement | Result |
|---|---|
| Reported and actual added latency, strict mode | **0 samples** at 44.1 and 48 kHz |
| Impulse response energy centroid | 0.00 samples |
| Output vs host block size (16 → 512) | **bit-identical** |
| Feedback tone attenuation | **−75.8 dB** |
| Level 400 Hz away from the cancelled tone | −0.15 dB |
| Tone cancelled from underneath a voice | −77.1 dB, voice within **0.17 dB** |
| Held sung note (false-positive test) | **0.00 dB** change, 0 detections |
| Voice spectrum, 100 Hz–6 kHz | worst band **0.82 dB**, mean **0.46 dB** |
| Mains hum, 50 / 60 Hz auto-selected | −85.2 / −81.9 dB |
| CPU, all stages on, 16-sample buffer | **3.1% of one core** per mono channel |
| 20 s of silence after hostile input | 8.8e−24 |
| Allocations on the audio thread, 2000 blocks + parameter sweeps | **0** |
| pluginval, strictness level 5 (VST3) | passes |

For context, published testing of Alpha Labs De-Feedback reported around 33% of a
core for one instance at a comparable buffer size.

---

## Installing

Builds are produced by GitHub Actions on every push. Open the latest run under
**Actions**, and download the artifact for your platform.

**Windows** — copy `FBKSuppressor.vst3` to
`C:\Program Files\Common Files\VST3\`.

**macOS** — copy `FBKSuppressor.vst3` to `/Library/Audio/Plug-Ins/VST3/` and
`FBKSuppressor.component` to `/Library/Audio/Plug-Ins/Components/`. The builds are
**not code-signed or notarised**, so Gatekeeper will block them until you clear
the quarantine flag:

```sh
xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/VST3/FBKSuppressor.vst3
xattr -dr com.apple.quarantine /Library/Audio/Plug-Ins/Components/FBKSuppressor.component
```

**Standalone** — run the binary directly. This gives the lowest achievable round
trip, because there is no host buffering in the path at all.

---

## Controls

| Control | Default | Notes |
|---|---|---|
| **Strength** | 100% | Overall dry/wet. At 0% the output is bit-identical to the input. |
| **Quality mode** | off | Linear phase, 128 samples of reported latency. |
| **Feedback** | on | |
| ├ Sensitivity | 50% | Relaxes or tightens every detection threshold together. |
| └ Depth | 100% | How much of a confirmed tone to cancel. |
| **Noise** | on | |
| ├ Amount | 50% | |
| ├ Max cut | 9 dB | Hard ceiling on per-band attenuation. Nothing can exceed it. |
| └ Protect | 80% | How strongly speech presence pulls a band's gain back to unity. |
| **Hum** | on | 50/60 Hz chosen automatically, harmonics locked to it. |
| **Dereverb** | **off** | The one stage that genuinely alters voice character. |
| **Rumble** | on, 35 Hz | 4th-order high-pass, below every vocal fundamental. |

Start with everything at default and only **Strength** in play. If a specific
feedback mode is getting through, raise Sensitivity before anything else.

### The capture button

Enable **Capture last 12 s**, and when something misbehaves press **Save WAV…**.
You get a two-channel file: channel 1 is the untouched input, channel 2 is the
processed output. That recording of your actual room and rig is far more useful
for tuning the detector than any amount of synthetic test material.

---

## Building

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/tests/fbk_tests
```

Expect the full plugin build to take several minutes: JUCE's recommended
link-time-optimisation flags are enabled, and LTO linking is slow and largely
serial. For iterating on the signal processing, build the core alone — it is a
JUCE-free C++20 library, so this configures, compiles and runs the whole test
suite in seconds:

```sh
cmake -S . -B build-dsp -G Ninja -DFBK_BUILD_PLUGIN=OFF
cmake --build build-dsp --parallel && ./build-dsp/tests/fbk_tests
```

Or just `./scripts/build.sh dsp`.

Linux plugin builds need: `libasound2-dev libx11-dev libxext-dev libxrandr-dev
libxinerama-dev libxcursor-dev libfreetype-dev libfontconfig1-dev libgl1-mesa-dev`.

---

## Known limitations

- **Sharp broadband transients pass through.** Structural, not a bug — see the
  table above. Quality mode helps a little; nothing at strictly zero latency will
  fix it.
- **No loudspeaker reference yet.** With the PA send as a sidechain, true adaptive
  feedback cancellation becomes possible, which removes *zero* voice energy rather
  than very little. The bus is declared and disabled; nothing reads it yet.
- **The broadband noise stage is heuristic.** A band-gain mask is inherently a
  compromise — pull a band down and every harmonic in it comes down together.
  Three constraints hold it in check (a hard attenuation ceiling, voice-presence
  protection, and decision-directed SNR smoothing to avoid musical noise), but
  this is the stage a learned separator will improve most.
- **macOS builds are unsigned.** See above.
- **Detector thresholds are defaults, not tuned to your room.** Use the capture
  button.
- **44.1 and 48 kHz are the designed-for rates.** Higher rates load and run — the
  band layout is derived per rate and pluginval exercises up to 96 kHz — but the
  analysis window is a fixed 2048 samples, so at 96 kHz it covers half the time
  span and the detector thresholds are no longer well matched. Not recommended
  without retuning.

---

## Roadmap

- **v0.2 — learned separator.** A small RTNeural model (compile-time-fixed
  topology, zero allocation, no thread pool) replacing the heuristic speech
  presence estimate. It will be asked only for a per-band *presence* value, never
  for output audio, so the zero-latency guarantee stays a property of the
  architecture rather than of the model.
- **v0.3 — sidechain AFC.** PEM-based adaptive feedback cancellation using the PA
  send, with the existing tonal canceller as the fallback when no reference is
  connected.

---

## References

Prior art and literature this was built against:

- [DeepFilterNet](https://github.com/Rikorose/DeepFilterNet) — two-stage ERB
  envelope plus complex deep filtering, 2.31M params, RTF 0.04 on a Core-i5,
  40 ms latency ([paper](https://arxiv.org/pdf/2305.08227),
  [DFN2](https://ar5iv.labs.arxiv.org/html/2205.05474))
- [RNNoise](https://jmvalin.ca/demo/rnnoise/) — 22 band gains, ~85k weights,
  10 ms frames, pitch comb filter
- [Alpha Labs De-Feedback V1](https://www.kvraudio.com/product/de-feedback-v1-by-alpha-labs)
  and [independent latency testing](https://newsletter.drewbrashler.com/p/alpha-labs-defeedback-testing-results)
- [Clear Voice Live](https://clearvoice.live/) — lookahead-free spectral
  separation at the host block size, zero reported PDC
- Howling detection criteria (PAPR / PHPR / PNPR / IPMP / IMSD) —
  [multi-criteria evaluation](https://www.sciencedirect.com/science/article/abs/pii/S0003682X21003704),
  [sparsity-based early detection](https://link.springer.com/article/10.1186/s13636-025-00399-1),
  [van Waterschoot & Moonen on feedback control](https://ftp.esat.kuleuven.be/pub/stadius/vanwaterschoot/downloads/presentations/oldenburg_20110201.pdf)
- Adaptive feedback cancellation and the PEM bias problem —
  [PEM-AFROW](https://www.researchgate.net/publication/228894432_Robust_and_Efficient_Implementation_of_the_PEM-AFROW_Algorithm_for_Acousic_Feedback_Cancellation),
  [switched PEM-NLMS](https://pmc.ncbi.nlm.nih.gov/articles/PMC8395445/)
- Minimum-latency filtering — [minimum-phase FIR design](https://www.katjaas.nl/minimumphase/minimumphase.html),
  [asymmetric analysis windows](https://arxiv.org/pdf/1606.09047)
- [RTNeural](https://github.com/jatinchowdhury18/RTNeural) — real-time-safe
  inference, chosen over ONNX Runtime for v0.2

See [`docs/DESIGN.md`](docs/DESIGN.md) for the reasoning behind each choice.

## Licence

Personal project, not for sale. JUCE is used under the GPLv3 option, which makes
distributed binaries GPLv3.
