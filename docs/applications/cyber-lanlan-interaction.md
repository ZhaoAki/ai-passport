<p align="right"><a href="cyber-lanlan-interaction.zh_CN.md">简体中文</a> · <strong>English</strong></p>

# Companion reactions and puppy sounds

Version 0.3 adds a shuffled four-reaction bag: blink, head tilt, happy raised paws
and open-mouth bark. Each round visits all four, and the next round excludes the
last reaction at its boundary. The application supplies `esp_random()` entropy to
an allocation-free pure selector. Its host test covers one million selections,
including constant entropy, verifying coverage and no adjacent duplicates.

On the companion page, each physical OK press reacts immediately. Subsequent
single/double-click classification events from that press are consumed to avoid
a duplicate reaction; fast presses are not collapsed into a fixed bark. Holding
OK still returns home after its initial press reaction. The first OK press after
screen-off only wakes the display, including its later click/long events.

Three original synthesized puppy-style vocalizations replace the old electronic
interaction chirp: a short arf, two arfs and a higher-pitched short arf. These are
not recordings of Lanlan or another real dog. The checked-in deterministic
harmonic/formant/noise synthesizer uses rounded envelopes and a peak limit of
16,000 in signed 16-bit PCM. The historical `chirp` identifier now contains the
two-arf variant; `bark_soft` is the third clip. The reminder chime stays unchanged.
Global mute suppresses audio without suppressing the visual reaction. A new press
replaces the active clip instead of queuing a backlog. Actual speaker quality and
rapid-press feel need device testing.

No care records, server endpoints, credentials, cache or partition layout change.
The sprite canvas remains 18,432 bytes of RAM, and the sound pack is 47,678 bytes
in Flash. Full build, artifact verification, host reaction/asset tests and UI
stress are required. HTTP tests remain blocked in the current environment by
socket permissions; passing firmware checks does not establish full acceptance.
