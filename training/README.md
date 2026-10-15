# Training the v0.2 separator

`NeuralSeparator` (in `src/dsp`) is an inference engine with no weights in the
repository. This directory is how you produce the weights.

| File | Purpose |
|---|---|
| `model.py` | `FbkNet`, the PyTorch twin of the C++ network. Same topology, same GRU convention. |
| `export_fbkm.py` | Writes a state dict as the `.fbkm` file `NeuralSeparator::load()` reads. Stdlib only. |
| `test_export.py` | Checks the exporter's byte layout without torch: `python training/test_export.py`. |

`model.py` has not been run in CI — there is no torch in the build environment.
What *is* verified, by the C++ suite, is the engine: the file format, the
arithmetic (against an independent double-precision reference, worst deviation
~1e-7), robustness to hostile input, zero allocation, and block-size invariance.
Before trusting a trained model, check one exported checkpoint's output against
`NeuralSeparator` on the same features; they should agree to ~1e-4.

## What the network is asked to predict

Per analysis frame (one 128-sample hop), per ERB band:

| Output | Target |
|---|---|
| `voicePresence[32]` | 1 where the band's energy is voice, 0 where it is anything else |
| `feedbackProb[32]` | 1 where the band holds a feedback tone, 0 otherwise |
| `voiceActivity` | 1 when a voice is present at all |

Voice presence and feedback probability are independent. A band can be neither
(steady room noise), and a sustained sung note must score **voice 1, feedback 0**
even though it looks tonal. Held sung notes, vibrato and falsetto are the hard
negatives for the feedback head; without them the model learns "tonal and steady
means feedback".

### Features

Computed identically in `NeuralSeparator::computeFeatures` — treat that function
as the definition. For each frame, from the 32 linear band energies `E[b]`:

```
dB[b]       = 10 * log10(E[b] + 1e-12)
level[b]    = clamp((dB[b] - max_b dB[b]) / 60, -1, 0)
delta[b]    = clamp((dB[b] - dB_prev[b]) / 20, -1, 1)     (0 on the first frame)
features    = [level[0..31], delta[0..31]]
```

Everything is relative to the loudest band, so the model is level-invariant: a
quiet howl looks like a loud one. The bands are the ERB layout in `ErbBands.cpp`,
defined in Hz, so one model serves 44.1 and 48 kHz. **Training features must come
from that layout** — the cleanest way to guarantee it is to dump band energies from
the C++ `Analyser`/`ErbBands` over your training audio rather than reimplement them.
(A small dump tool for this is not written yet.)

### Conditioning

The `control` input is `{feedback sensitivity, feedback depth, max cut / 18 dB,
voice protection}`, each in `[0, 1]`, the same four values the plugin passes at
runtime. Sample them randomly per training sequence so one model covers the whole
control space.

### Data

The labels the feedback head needs are exactly what `FBKTrainer` is built to
produce: a speech corpus played through a real PA and room, with the loop gain
known at every instant, so a frame can be labelled "feedback present" or "heading
to feedback" from measurement rather than by ear. Add synthetic howl (growing
sinusoids at room-mode frequencies) over recorded voice for volume, and keep the
sung-note negatives described above.

### Losses

Binary cross-entropy on each head against its target, with the feedback head's
false-positive term weighted up. The cost asymmetry is the same one the detector is
built around: a missed howl is briefly unpleasant, a false positive subtracts part
of someone's voice. (The runtime already guards against the latter — see below —
but a model that is right is better than a model that is merely contained.)

## What a wrong model can and cannot do

The model's feedback probability is advisory. `HowlDetector::setLearnedPrior`
lets it lower the ratio and persistence evidence a candidate needs, by at most what
a profiled room mode already gets, and it never touches the frequency-stability
(FSD) or slope-deviation (IMSD) criteria. The C++ suite feeds the suppressor
arbitrary large-weight models and asserts that a held sung note is left untouched
and produces zero confirmed tones. So a badly trained model costs speed or voice
colour in the mask, not voice to a canceller.

## Sizes

The engine accepts hidden sizes up to 512. A model with hidden 64, trunk 16 and
smoother 32 is about 47k parameters, so roughly that many multiply-adds per hop;
that should sit well under the existing DSP cost, but it has not been measured —
measure it
on the target machine — `testRealTimeFactor` is the place to add a model-attached
case once real weights exist.

## Loading a model

```cpp
fbk::NeuralSeparator sep;
auto r = sep.load (bytes, size);     // not on the audio thread
if (r.ok) suppressor.setSeparator (&sep);
```

`load()` allocates, so call it before attaching, never while the separator is
processing. The plugin does not load a model file yet; wiring that (a file chooser
and a state-saved path) is the remaining v0.2 plugin work.
