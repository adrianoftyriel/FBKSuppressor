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

From the test suite (`./build/tests/fbk_tests`, 412 checks, all passing under
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
| Room-mode priors: tone energy leaked at onset | **−33%** |
| Sweep round trip vs known room (delay / RT60 / reflection) | 201 vs 200 samples / 0.394 vs 0.400 s / 0.41 vs 0.50 |
| pluginval, strictness level 5 (VST3) | passes |

For context, published testing of Alpha Labs De-Feedback reported around 33% of a
core for one instance at a comparable buffer size.

---

## Releases

| Branch | Publishes | Tag |
|---|---|---|
| `dev` | **pre-release** | `v0.1.0-dev.<run number>` |
| `main` | **release** | `v0.1.0` |

Every push to either branch builds all three platforms, runs the DSP suite on each
of them, validates every VST3 with pluginval, and only then publishes. The release
carries the exact binaries that were tested — publishing happens in the same run as
the build rather than in a separate workflow, so there is no way for the two to
drift apart.

Each release contains a zip per platform plus `SHA256SUMS.txt`, and notes with
install instructions and the changelog since the previous release of the same kind.

**The version lives in `CMakeLists.txt`** (`project(FBKSuppressor VERSION x.y.z)`)
and is the single source of truth. Pre-release tags append the run number, so `dev`
never needs a bump. A **release** does: if `v<version>` already exists, the publish
step fails with a message rather than overwriting binaries you may already have
downloaded. So the flow for a real release is bump the version in `CMakeLists.txt`,
merge to `dev`, check the pre-release, then merge to `main`.

Publishing can also be triggered manually: **Actions → Build → Run workflow**, on
`dev` or `main`. That exists as a recovery path — the publish step depends on the
validation jobs, and a run can occasionally stall between those finishing and the
dependent job being scheduled, leaving every job green but nothing published. Gated
on pushes alone there was no way to publish that commit without inventing a new one.
It also means you can cut a release on demand.

If a run ever ends with all jobs green and no release, that is what happened, and
either a manual dispatch or "Re-run all jobs" will finish it. The build artifacts are
downloadable from the run itself in the meantime — that separation is deliberate.

Note that pre-releases accumulate — one per push to `dev`. Nothing prunes them
automatically, because deleting releases is not something I'll do unasked; say the
word if you want old ones cleaned up on a rolling basis.

## Installing

Grab the latest [release or pre-release](../../releases), or for an untagged build,
open a run under **Actions** and download the artifact for your platform.

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

### Standalone driver backends

The standalone is the only build that talks to a driver itself — a plugin inherits
whatever the host is using — so the low-latency backends matter here and nowhere
else. Both are enabled explicitly, because JUCE has them **off by default**:

| Platform | Backends offered |
|---|---|
| Windows | **ASIO**, plus WASAPI (shared, low-latency shared, and exclusive) and DirectSound |
| macOS | CoreAudio |
| Linux | **JACK**, plus ALSA |

Use ASIO on Windows if you have any interface at all — WASAPI exclusive is the
fallback and can manage a few milliseconds, but it will not reach the 16–64 sample
buffers this plugin is designed around.

ASIO appears as a device *type* even when no ASIO driver is installed, in which
case its device list is empty. That means no driver, not a broken build — install
your interface's driver, or ASIO4ALL as a generic fallback (a real interface driver
will always do better).

---

## Controls

The window is laid out like a channel strip, because that is what your hands
already know. Across the top: identity, bypass, quality mode, the latency
readout and a running status line, then the three tabs. Below that the analyser
takes a third of the height, and the bottom half is five module strips —
Feedback, Noise, Hum, Dereverb, Rumble — each with an illuminated switch, a live
readout, and one vertical fader per parameter. **Strength** is the tall fader
down the right-hand side, running from the top of the analyser to the bottom of
the window. It sits outside the tabs on purpose: the master control has to be
under your hand whichever tab happens to be open.

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
feedback mode is getting through, raise Sensitivity before anything else — or
better, calibrate (below), which sets the thresholds from measurement instead.

## Calibrate tab

Every detection threshold shipped as a default was chosen against a *synthetic*
voice. That is the weakest part of the design, because those thresholds decide
whether a canceller engages on a vocal harmonic. Calibration replaces the guesses
with measurements of your voice in your room.

Run each phase once, in order. **Nothing changes until you press Apply.**

| Phase | What you do | What it measures |
|---|---|---|
| **1. Room noise** (15 s) | Everyone quiet, PA at show gain | Per-band noise floor; sets the absolute peak gate in real dBFS instead of a guess |
| **2. Voice** (45 s) | Talk and sing across your range, PA down | Long-term average spectrum, f0 range, and a histogram of what each detection criterion *does* on your voice |
| **3. Room modes** (90 s) | Push gain past show level, slowly | The frequencies the suppressor actually has to fight |

Phase 2 is the important one. It histograms PNPR, PHPR, peak prominence and
frequency stability across your voice, then places each threshold just outside
that distribution — so a feedback tone has to be more tone-like than anything your
voice has ever produced. During this phase the detector is in observe-only mode
and **cancels nothing**, so it cannot act on the signal it is characterising.

On the synthetic test voice, the shipped 2.5 Hz frequency-stability threshold
turned out to be five times looser than necessary; calibration tightens it to
0.4 Hz, which catches feedback sooner while still rejecting that voice. The
prominence gate, by contrast, came out at 12.2 dB against a hand-picked default of
12 dB — so some guesses were fine and some were not, which is rather the point of
measuring.

**Phase 3 is not ringing out.** Cancellation stays fully **on** while you raise
the gain — being protected is what makes pushing it safe. The plugin logs every
mode it had to kill, and those frequencies become *priors*: at a known-suspect
frequency the detector needs less evidence before it confirms, so it engages
faster there. Measured effect: **33% less tone energy leaks through at onset**.
No EQ is applied, nothing is cut, and cancellation still only happens when a tone
is genuinely present.

Profiles save and load as JSON. Every value in them is in Hz or dB rather than
bins or samples, so a profile measured at 48 kHz is still valid at 44.1 kHz (the
plugin says so when the rates differ).

## Diagnostics tab

**Telemetry** logs band energies and every detection — with all six criterion
values — at 25 Hz to CSV. Roughly 50–70 MB per hour as text, against about 1 GB
per hour for dry+wet audio, and far more useful: when a tone gets through, the
question is never "what did it sound like" but "which criterion failed", and only
one of these two records can answer that. The panel shows actual rows written, so
you never have to trust my estimate. The audio thread only pushes fixed-size
frames into a lock-free ring; every file operation happens on a background thread,
and if that thread ever fell behind, frames are dropped and counted rather than
stalling the audio.

**Rolling capture** keeps the last 12 seconds and saves it automatically when
something happens — a new detection, or the difference signal (input minus output)
exceeding a threshold you set. That second trigger is the one that matters most
and is hardest to notice by ear: it fires when the plugin is doing something
substantial, which is exactly the case to review if you suspect it is acting on
the voice. You get a two-channel file: channel 1 untouched input, channel 2
processed.

**Feedback path measurement** emits an exponential sine sweep to the PA and
deconvolves the microphone return into the impulse response of the whole loop —
console, PA, room, mic. Exported as WAV. This is what makes realistic closed-loop
feedback simulation possible for v0.2 training, rather than approximating with
public impulse responses. It reports loop delay, RT60 and peak level, and warns
you if the take clipped or was too quiet.

Verified against a synthetic room: a 200-sample loop delay came back as 201, an
RT60 of 0.400 s measured as 0.394 s, and a reflection at half amplitude recovered
at 0.41.

It emits full-band audio at a level you choose and requires an explicit
confirmation. Don't do it with an audience in the room.

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
- **Detector thresholds are defaults until you calibrate.** The Calibrate tab
  exists precisely to fix this; until you run it, every threshold is a guess made
  against a synthetic voice.
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
