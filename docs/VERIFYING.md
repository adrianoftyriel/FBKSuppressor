# Verifying the claims yourself

Everything below is something you can check in your own DAW in a few minutes. A
zero-latency claim is easy to make and easy to test, so please test it.

## 1. Zero added latency (the important one)

**The null test — proves the plugin is honest about reporting.**

1. Put a short click or a drum hit on a track.
2. Duplicate the track. Put FBKSuppressor on one copy.
3. Set **Strength** to 0%.
4. Invert the polarity of one track and sum them.

They must null to **digital silence**. At Strength 0 the plugin's output is
bit-identical to its input — this is asserted in the test suite — so anything you
hear is your DAW's delay compensation being wrong, not the plugin's.

**The impulse test — proves nothing is delayed.**

1. Record a single-sample click through the plugin at Strength 100%, with the
   Rumble filter switched off (a high-pass has its own group delay by design).
2. Zoom to sample level and find where the response begins.

It must begin on **the same sample** as the input click. Not one sample later.
In Quality mode it should begin exactly 128 samples later, and your DAW should be
compensating that for you — the header display tells you which mode you are in and
what is being reported.

## 2. Feedback suppression, without risking your ears

Do not ring out a real PA to test this. Instead:

1. Feed a signal generator's sine tone into a track, at a level well above the
   noise floor, at some frequency like 2137 Hz.
2. Insert the plugin. Watch the analyser: within about a second a red line should
   appear at that frequency with its value labelled.
3. Look at the output on a spectrum analyser. The tone should drop by 40 dB or
   more.

Then the part that actually matters:

4. Play a vocal recording *through* the same tone.
5. Confirm the tone still disappears and the voice does not change.

Compare against inserting a narrow notch at the same frequency — the difference in
what happens to the voice is the entire argument for this approach.

## 3. Voice character preservation

The honest test is a null test against the dry signal, on clean vocal material
with no feedback and no significant noise:

1. Duplicate a clean vocal track, plugin on one copy, Strength 100%.
2. Invert one and sum.

What is left is exactly what the plugin is doing to your voice. On clean material
it should be very quiet and should not sound like the voice — if you can clearly
hear intelligible speech in the difference, the noise stage is working too hard.
Turn **Amount** down, or **Protect** up, or reduce **Max cut**.

This is the single most useful measurement for the stated goal, and it is worth
doing with your own voice on your own microphone before any show.

## 4. False positives on sustained notes

Have someone hold a long, loud note — the case most likely to be mistaken for
feedback. Watch the tone indicators in the analyser. Nothing should be confirmed.

The test suite covers this with a synthetic held note (0.00 dB change, zero
detections), but a synthetic voice is not a real one. If you *do* see detections
on held notes, lower **Sensitivity** and send me the capture.

## 5. CPU headroom

The header shows measured CPU as a percentage of real time for the current block.
Check it at the buffer size you will actually run, with every instance you will
actually run, before the show rather than during it.

The test suite measures 3.1% of one core per mono channel at a 16-sample buffer
with all stages active, but your machine, buffer size and channel count are what
matter.

## 6. When something goes wrong

Turn on **Capture last 12 s**. When you next hear the problem, hit **Save WAV…**
straight afterwards. You get channel 1 = untouched input, channel 2 = processed
output, which is enough to work out exactly what the detector saw and why it did
what it did.
