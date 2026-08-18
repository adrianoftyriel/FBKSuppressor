# Design notes

Why each choice was made, including the ones that turned out to be wrong.

---

## 1. The problem with every off-the-shelf option

The two obvious starting points both fail the brief for the same structural
reason.

**RNNoise** splits the spectrum into 22 Bark-ish bands and uses three small GRUs
(~85k weights, 42 input features) to predict a gain per band, with a pitch comb
filter cleaning up between harmonics. It runs on 480-sample frames at 48 kHz and
looks ahead about 10 ms.

**DeepFilterNet2** predicts 32 ERB-scaled gains to reconstruct the speech
envelope, then applies a complex 5-tap filter to the lowest 96 bins (up to
4.8 kHz) to clean up the periodic structure. 2.31M parameters, real-time factor
0.04 on a Core-i5 and 0.42 on a Raspberry Pi 4. It uses a 20 ms window with a
10 ms hop and two frames of lookahead: **40 ms total latency**.

Two problems, and the second is worse than the first.

**Latency.** 40 ms is unusable in a live monitor path. 10 ms is marginal.

**Band gains dull a voice.** This is the deeper issue and it is inherent to the
approach, not a tuning problem. An ERB band is wide — hundreds of hertz in the
formant region. When a feedback tone and a vocal formant share a band, a
band-gain mask pulls both down together. DeepFilterNet's second stage exists
precisely to work around this, and that stage is where the lookahead goes.

Both are also **48 kHz only**. Resampling 44.1 kHz up to 48 k and back would add
latency, which defeats the exercise entirely.

## 2. What the commercial plugins are actually doing

Alpha Labs De-Feedback V1 markets itself as zero-latency; independent testing
confirmed no added plugin latency, with the measured ~4.83 ms round trip at a
16-sample buffer being interface and console I/O (an X32 aux path contributing
~0.9 ms). The published mechanism is explicitly subtractive: identify the
non-voice content, invert its polarity, sum it back. Reported outcome: ~10 dB
more gain before feedback. The cost is CPU — around 33% of a core for one
instance, which is why dedicated hardware is sold alongside it.

Clear Voice Live claims 0.0 ms internal latency with the AI core processing
"lookahead-free at the host's block size" and zero reported PDC.

Both descriptions are consistent with one architecture: **analyse the past,
predict the interference, subtract it, never delay the output.** That is what
this project implements.

## 3. The zero-latency mechanism

Three pieces.

**Trailing analysis-only STFT.** A 2048-point window covering the most recent
2048 samples, ending at the newest sample the host has given us. There is no
resynthesis anywhere. Overlap-add is what forces a window-length delay, because
the output at a given instant is not complete until every overlapping window has
been summed in — so by never resynthesising, we get a long window's frequency
resolution for free.

The window is **asymmetric**: a long half-Hann rise over the first 7/8 and a short
half-Hann fall over the last 1/8. A symmetric window puts its centre of mass
~21 ms in the past, which makes the estimate sluggish exactly when a feedback tone
is ramping up. The asymmetric shape pulls the measurement centroid to ~14.8 ms
while keeping sidelobes low enough to resolve a howl from a vocal harmonic.
Because there is no resynthesis there is no COLA constraint, so the window can be
shaped purely for measurement quality.

**Minimum-phase mask realisation.** The broadband mask is treated as the magnitude
response of a filter and realised directly in the time domain. Of all filters with
a given magnitude response, the minimum-phase one has the smallest group delay —
its impulse response is causal with its energy packed into the first taps. It is
computed by the standard cepstral method: real cepstrum of the log magnitude, fold
the anti-causal half onto the causal half, exponentiate back, truncate to 64 taps
with a short taper. Measured energy centroid of the realised response: **0.00
samples**.

The cost is non-linear phase. For a magnitude response this smooth (32 ERB bands,
interpolated between centres, limited to 9 dB) it is inaudible on speech. Quality
mode offers linear phase for 128 samples if you would rather.

**Subtractive tonal cancellation.** For each tracked tone, a unit-amplitude complex
oscillator runs at the tone's frequency and a complex weight (amplitude and phase)
adapts so the scaled oscillator matches the sinusoidal component present in the
input. That synthesised sinusoid is subtracted. What is removed is one specific
coherent waveform; anything else at the same frequency survives, because it does
not correlate with the oscillator over the adaptation window.

The effective notch bandwidth is `mu * fs / (4*pi)` — about 4 Hz at the step sizes
used, a Q in the hundreds to thousands. A vocal harmonic moves through tens of
hertz from vibrato and pitch contour alone and never sits still long enough to be
learned. A feedback tone does not move and is learned in a few tens of
milliseconds.

## 4. Cost, and why not GPU

Measured: **4.4% of one core** per mono channel with every stage enabled, at a
16-sample buffer — and flat from 16 to 512 samples, which confirms there is no
per-block overhead. The chain is driven sample-by-sample off an internal hop
counter, so host buffering is not observable in the output at all (verified
bit-identical across block sizes).

The two things that make this affordable:

- The analysis runs on a 128-sample hop, not per block. At a 16-sample buffer,
  running a full analysis per block would be 8× the work for no benefit.
- The broadband mask is redesigned every 4 hops (~10 ms) and its coefficients are
  interpolated between designs. Only the tone tracker needs to be fast; a noise
  floor does not move in 2.7 ms.

**GPU and NPU were considered and rejected for the main path.** At 16–64 sample
buffers, per-buffer transfer and kernel-launch overhead exceeds the entire compute
budget, and it introduces jitter that cannot be bounded — the general finding that
GPU plugin acceleration loses to native code at small buffer sizes. For a workload
this small, SIMD on one core is both faster and deterministic. Clear Voice Live's
DirectML path is plausible only at large buffers.

## 5. Detection: the published criteria, and where they were not enough

Howling detection uses conjunctions of independent criteria, from the literature:

- **PAPR** — peak-to-average power. Feedback gets loud. A loud vowel passes too,
  so this is only ever a gate.
- **PNPR** — peak-to-neighbouring power. A howl is an undamped sinusoid with
  essentially zero bandwidth. Measured at ±3–6 bins, outside the window's main
  lobe.
- **PHPR** — peak-to-harmonic power, against subharmonics at k/2 … k/5. A vocal
  harmonic has energy below it; an isolated tone does not.
- **IPMP** — interframe peak magnitude persistence.
- **IMSD** — interframe magnitude slope deviation. Exponential buildup is a
  straight line in dB, so the *deviation* of the frame-to-frame slope is small
  while the slope is positive.

The comparative studies find pairwise conjunctions — PHPR & IPMP, PNPR & IMSD —
sufficient. **In testing here they were not.** A held sung note produced nine
confirmed "feedback" tones. The reason: PHPR only looks at integer subharmonics,
so a *prime-numbered* harmonic of a voice (the 5th, 7th, 11th…) has nothing below
it and scores exactly like an isolated tone, while sailing through PAPR and IPMP.

Two changes fixed it:

**A frequency-stability criterion (FSD).** An acoustic feedback mode is fixed by
the room and the loop; its measured frequency moves by well under a hertz between
frames. A voice is in constant motion — tens of hertz on the upper harmonics from
vibrato and contour. Measured from the sub-bin parabolic peak estimate, so the
resolution is far better than the 23 Hz bin spacing. This is decisive and cheap.

**Requiring the full conjunction.** The cost asymmetry here is severe: a missed
howl is briefly unpleasant, a false positive means quietly subtracting part of
someone's voice. So all criteria must hold, and the sensitivity control relaxes
thresholds rather than the logic. Result: the held note now measures **0.00 dB**
change with zero detections, while a real tone is still caught and cancelled at
−75 dB.

Because the canceller's adaptation rate scales with detection confidence, even a
false positive that did slip through could only act slowly and narrowly before
the detector dropped it.

## 6. Six bugs the tests found

Worth recording, because each was a design error rather than a typo, and none
would have been obvious by reading the code.

**Plain LMS diverged on hostile input.** A single 1e30 sample drove the weight
update into runaway oscillation; the processor was still producing garbage
(output RMS 2.9e23) a full second of normal audio later. Fixed by
power-normalised NLMS plus a hard weight bound — which also made convergence
level-independent, so a quiet howl is now caught as fast as a loud one, and made
the step size a dimensionless number that maps directly onto a notch bandwidth.
Input sanitisation was added at the plugin boundary as well.

**The peak gate rejected the thing it was looking for.** Candidates were gated
against the minimum-statistics noise estimate. But minimum statistics tracks the
running minimum of each bin, and a sustained feedback tone *is* stationary — so
after a second or two the "noise floor" at the howling bin rose to meet the howl
and the gate silently dropped it. Measured: tracked for 838 frames, then gone,
with attenuation collapsing to 0.4 dB. Replaced with a **local spectral floor** — a
moving average over ±24 bins by running sum, O(N) — which asks the right question
(does this peak stand out from its neighbourhood?) and which a single narrow peak
cannot corrupt, since it contributes ~2% of a 49-bin average. It discriminates for
free, too: an isolated tone towers over its neighbourhood, whereas a vocal
harmonic sits in a comb of similar peaks that raises the floor around it.

This one is also a general warning: minimum statistics will happily classify a
sustained howl as noise and hand a whole ERB band to the mask. That is another
reason the subtractive stages run first.

**Published detection criteria confirmed vocal harmonics.** Covered above.

**Toggling Quality Mode allocated on the audio thread.** Switching phase mode
changes the filter length, and the mode setter resized four coefficient vectors to
match. That setter is reachable from `setParameters()`, which the plugin calls from
`processBlock` — so every press of the Quality Mode button allocated on the audio
thread, and an allocation can take a lock. Fixed by sizing every tap buffer once
at the largest mode's length and treating a mode change as a change of tap count
plus a fill.

This one is worth dwelling on because reading the code was never going to find it:
the allocation was three calls below the parameter setter. What found it was a test
that replaces global `operator new`, arms a counter, and then runs 2000 blocks
while sweeping every parameter and toggling the phase mode — asserting zero
allocations. Real-time safety is now verified rather than asserted. (Two
`thread_local` metering scratch buffers were caught by the same test; a function-
local static allocates on first use, and first use was on the audio thread.)

**`std::numbers` is not portable enough.** `<numbers>` is standard C++20 and GCC 13
has it, but Apple Clang's libc++ on the macOS CI runner does not — so the build
failed on macOS having passed on Linux. Replaced with a plain constant in
`Common.h`. Two consequences: the CI now runs the DSP tests under both GCC and
Clang as a fast gate, and it runs them natively on all three platforms rather than
trusting Linux to speak for the others.

**Hum probes built on LMS weights measured the wrong thing.** Deciding between
50 and 60 Hz cannot be done from the analysis FFT — at 23 Hz bin spacing,
consecutive hum harmonics are barely two bins apart. The first attempt ran two
pinned LMS oscillators and compared their weights, but an oscillator at 60 Hz fed
a 50 Hz signal produces a weight beating at 10 Hz with a large instantaneous
magnitude, because the adaptation time constant is comparable to the beat period.
Measured: the 60 Hz probe scored *higher* than the 50 Hz probe against a pure
50 Hz harmonic series. Replaced with **single-bin DFTs using an exponential
window** — a one-pole complex accumulator, 0.5 s time constant, ~0.3 Hz bandwidth,
which puts the wrong candidate 30 dB down. Separation is now 0.82 vs 0.026.

(An earlier iteration also let the probes' phase-locked loops run, which simply
slid the wrong probe onto the right frequency. PLL range is now a per-canceller
setting: zero for probes, ±0.5 Hz × harmonic number for hum, ±20 Hz for feedback
tones.)

## 7. Native 44.1 and 48 kHz

The analysis FFT is a fixed **2048 samples** at both rates rather than a fixed
duration, so it stays a power of two at 44.1 kHz. The resulting bin spacing —
21.5 Hz versus 23.4 Hz — is close enough that one set of detector thresholds
serves both.

Band edges are specified in **Hz on the ERB scale** and mapped onto the FFT grid
per rate at `prepare()`. Every time constant is specified in milliseconds and
converted at `prepare()`. Nothing downstream has a per-rate code path, and no
resampler is ever inserted.

## 8. Why the analyser watches the input, not the output

If the detector watched the processed output it would see a successfully
cancelled tone as absent, disengage, and let the feedback straight back in —
oscillating between suppressed and not. Watching the input means the canceller
stays engaged for exactly as long as the loop is still trying to ring.

The same logic applies to the hum probes: they run on the unmodified input,
before the harmonic cancellers, so they cannot switch off their own detection as
soon as it succeeds.

## 9. Stage order

1. High-pass (35 Hz, optional) — subsonic rumble only, below every vocal
   fundamental. This is the one genuinely frequency-subtractive element, and it
   is deliberately placed where no voice lives.
2. Hum canceller — subtractive, exact harmonic series.
3. Tonal canceller — subtractive, tracked feedback tones.
4. Mask filter — broadband noise and optional dereverb, as a causal
   minimum-phase FIR.
5. Strength mix — with the dry path delay-matched, so Strength is a true
   crossfade in Quality mode rather than a comb filter.

The subtractive stages come first because they are surgical. Running them before
the mask also means the mask estimator never sees a howl in its noise statistics,
where it would drag a whole ERB band down and take vocal harmonics with it.

## 10. Voice-colour constraints on the mask stage

A band-gain mask is, by construction, the thing that can dull a voice. Three
constraints:

- **A hard attenuation ceiling** (9 dB by default). No band can be gutted no
  matter what the estimator concludes.
- **Voice-presence protection.** Where speech presence is high the gain is pulled
  back towards unity, so the mask works in the gaps rather than on the voice.
- **Decision-directed a-priori SNR** (Ephraim-Malah, 0.94 weighting on the
  previous frame). A per-frame estimate here is the classic cause of musical
  noise and the "underwater" artefact that makes processed voices sound
  synthetic.

Measured on a synthetic voice with vibrato and pitch contour: worst band 0.82 dB,
mean 0.46 dB across 100 Hz–6 kHz.

This is still the weakest stage, and it is what v0.2 targets.

## 11. v0.2: the learned separator

`SpectralSeparator` is already in the tree with a null implementation. The
intended model is small, RTNeural-based, and takes the 32 ERB band energies the
mask estimator already computes plus a small feature set, returning per-band
voice presence to replace the heuristic estimate.

**RTNeural over ONNX Runtime**, despite ONNX being faster on stateless models:
RTNeural fixes topology and weights at compile time, giving genuinely
zero-allocation, wait-free execution. ONNX Runtime carries internal thread pools
and can allocate during a forward pass — nondeterminism that shows up as
dropouts.

The critical architectural constraint: **the network is only ever asked for a
presence estimate, never for output audio.** Audio still leaves through the
minimum-phase filter. That keeps the zero-latency guarantee a property of the
architecture rather than of the model, so no amount of retraining can break it.

Model input must be defined in Hz-relative terms so one set of weights serves
both sample rates.

Training needs the DNS-Challenge corpus (~1 TB unpacked: 827 GB clean speech,
58 GB noise, 5.9 GB impulse responses, plus 60k RIRs from SLR28) with closed-loop
feedback simulation, and GPU hours. Both are out of scope for the v0.1 build.

## 12. v0.3: sidechain AFC

With the loudspeaker signal available as a reference, the feedback path itself can
be estimated and subtracted, which removes *zero* voice energy rather than very
little. Plain NLMS converges to a biased estimate because speech is spectrally
coloured and correlated with the loudspeaker signal; PEM-based variants fix the
bias at the cost of convergence speed, and switched schemes select between them on
a stability detector. That is the intended implementation, with the existing blind
tonal canceller as the fallback whenever no reference is connected.

The bus is declared and disabled in `PluginProcessor.cpp`. Nothing reads it yet.
