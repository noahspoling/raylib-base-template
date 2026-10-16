# Audio Service

Procedural sound: every effect is synthesized at play time from a small flat
description, so one footstep call can vary with surface, foot and weight
instead of picking from a pile of samples.

## File map

| File | Role |
|---|---|
| [synth.h](../../../include/services/audio/synth.h) / [synth.c](synth.c) | DSP + voice pool + command queue. Pure C, no raylib — testable offline |
| [audio_device.c](audio_device.c) | raylib `AudioStream` callback, scope / spectrum debug views |
| [synth_lua.c](synth_lua.c) | `gramarye.audio.*` bindings (desc table <-> `SynthVoiceDesc`) |
| `assets/scripts/lib/sfx.lua` | materials, presets, footstep generator |
| `assets/scripts/scenes/synth.lua` | the lab: every parameter on a slider |

## Threading

```
game thread                         audio thread (miniaudio, via raylib)
synth_play / update / release  ──►  SPSC ring (128 cmds)  ──►  synth_render
                                                               voices -> mix -> soft clip -> scope ring
```

Only commands cross threads. The ring uses acquire/release atomics; the voice
pool (32), coefficients and filter state belong to the audio thread. The scope
ring is read racily by the debug views, which is fine for a picture. When the
queue is full `synth_play` returns 0 rather than blocking.

## One voice

```
osc (sine/tri/square/pulse/saw, PolyBLEP) ─┐
   pitch env + LFO                         ├─ x amp ADSR = source
noise (colored, optionally grain-gated) ───┘
source ── x dry ─────────────────────────────┐
source ── x excite ─► modal bank (<= 6) ─ x modes_level ─┴─► SVF (lp/hp/bp + env) ─► drive ─► gain/pan
```

- **Modes** are two-pole resonators (`decay` = T60). A burst of a few ms rings
  them like a strike; their frequency *ratios* are what make wood, metal or
  glass sound like themselves (`sfx.materials`).
- **Grains** gate the noise into random clicks (`grain_rate`/s, each decaying
  over `grain_len`). This is what turns noise into gravel, snow or rain.
- **Jitter** randomizes pitch, modes, cutoff and gain per note from `seed`
  (or the handle), so repeated sounds never machine-gun.
- **Hold** < 0 keeps the gate open until `synth_release`. `delay` schedules
  layers (heel then toe, splash droplets) without game-side timers.
- A voice frees itself once the envelope is done and its resonators have
  rung out.

## Adding things

- **New sound:** add a table to `sfx.presets`, or tune one in the lab and
  click *Log patch*.
- **New material / surface / foot:** data in `sfx.lua`; F5 reloads it.
- **New DSP block:** add the fields to `SynthVoiceDesc`, derive coefficients in
  `voice_apply_desc` (never per sample), process in `voice_render`, then
  expose them in `read_desc` / `push_desc` in `synth_lua.c`.

## Gotchas

- Web: browsers start audio suspended until the first user gesture.
  miniaudio resumes it on input, so the first click or key may be silent.
- Output is soft-clipped, so many loud layers compress instead of wrapping.
  If things sound squashed, lower gains rather than master.
