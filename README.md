# Voice Morph

A voice pitch effect for Akai MPC standalone hardware, by Germanikkus. Put it as an insert on a track carrying a vocal (live mic input or a vocal sample) and pick a Style.

Requires MPC OS 3.10.1 or later with Preferences → Enhancements (Beta) → **Enable Enhancements** and **External Plugin Support** turned on.

## Styles

| Style | What it does | What **Character** does |
|---|---|---|
| Natural | Gently pulls each note into the Key/Scale. | Speed: 0 … 240 ms (50 % = 60 ms) |
| Hard Tune | Snaps to the nearest scale note: the stepped, robotic pop sound. | Instant up to 50 %, then softens up to 60 ms |
| Robot | Every sung note lands on one note: a flat monotone robot. | Note: 5th Down / Root / 5th Up |
| Chipmunk | Hard-tuned an octave up with a smaller, brighter voice. | Size: -4 … +4 st on top of the style's own |
| Monster | Hard-tuned an octave down with a huge, dark voice. | Size: -4 … +4 st (more = bigger) |
| Harmony | Hard-tuned lead plus a second voice inside the scale. | Interval: Oct Down, 3rd Down, 3rd Up, 5th Up, Oct Up |
| Warble | Hard-tuned with a 5.5 Hz vibrato. | Wobble depth |
| Underwater | Muffled, slowly sweeping resonant filter with a drifting, chorused voice. | Murk: how deep |

The Character knob's value text always says what it is doing (for example `Speed 60 ms`, `3rd Up`, `Murk 60%`).

## The screen (one page, four columns)

Each column is one Q-Link bank, so the four knobs next to a column edit that column.

| VOICE | TUNE | FX | EQ |
|---|---|---|---|
| Style | Character | Echo | Low Cut |
| Key | Sens | Echo Time | Lows |
| Scale | Mix | Stutter | Highs |
| Formant | Output | Width | High Cut |

| Control | Range | Notes |
|---|---|---|
| Style | 8 styles | see above |
| Key | C … B | scale root; also the Robot note |
| Scale | Chromatic, Major, Minor, Pent Maj, Pent Min, Blues | Chromatic = every semitone |
| Formant | -6 … +6 st | voice size without changing pitch |
| Character | 0 … 100 % | depends on Style, see above |
| Sens | 0 … 100 % | pitch detector sensitivity: raise it if notes drop out, lower it if breaths or noise get tuned |
| Mix | 0 … 100 % | dry/wet of the pitch effect |
| Output | -24 … +12 dB | final level |
| Echo | 0 … 100 % | ping-pong echo; one knob raises level and repeats together. 0 = off |
| Echo Time | 1/16, 1/8T, 1/8, 1/8D, 1/4T, 1/4, 1/4D, 1/2 | synced to the MPC's tempo (120 BPM if it sends none) |
| Stutter | 0 … 100 % | chance that each 1/16 step turns into a repeat of 1/32 to 1/128 slices; follows the song position while playing |
| Width | 0 … 100 % | stereo spread of the voice; mono-compatible (the two sides sum back to the original) |
| Low Cut | 20 … 1000 Hz | removes rumble and boom below this (12 dB/oct). 20 Hz = off |
| Lows | -12 … +12 dB | low shelf at 200 Hz: down for less mud, up for more body |
| Highs | -12 … +12 dB | high shelf at 4 kHz: down for less hiss and harshness, up for more air |
| High Cut | 2 … 20 kHz | removes fizz and hiss above this (12 dB/oct). 20 kHz = off |

Signal order: pitch effect → Mix → Width → Stutter → Echo → EQ → Output.

Ten built-in presets: one per style, plus **Vocal Throw** (Hard Tune with a dotted-1/4 echo) and **Glitch Bot** (Robot with stutter).

Parameters `Old Speed`, `Old Harmony`, `Old Wobble` and `Old Murk` are left over from 1.0/1.1 (Character replaced them). They do nothing and are not on screen; they stay so saved projects keep their parameter numbering.

## How it works

A YIN pitch detector runs on a decimated copy of the input every ~5.8 ms (voices from 70 Hz to 1000 Hz). A pitch-synchronous overlap-add (PSOLA) shifter cuts two-period grains at the detected pitch and lays them down at the corrected pitch, which keeps the voice's character; grain playback speed sets Formant. Breaths and consonants pass through unshifted (or octave-shifted in Chipmunk/Monster).

## Honest notes

- Latency is about 14 ms (one lowest voice period), reported to the host.
- The pitch effect is mono; Width and Echo spread it in stereo.
- One voice at a time: chords, backing tracks or heavy reverb on the input confuse the pitch detector. Put it on a dry vocal.
- Tested on the computer with the SDK host simulator and offline renders (echo lands on the exact 1/4 at 120 BPM; stutter and echo add no clicks). Not yet checked on hardware: whether the MPC sends tempo and song position to insert effects (if not, Echo Time and Stutter run at 120 BPM).

## Build

This project lives on its own (`~/Germanikkus/voice-morph`) and is built with the MPC Plugin Builder SDK, checked out next to it at `~/Germanikkus/mpc-plugin-builder-sdk`. Run these from this folder:

The page uses a backdrop image, which the SDK's ui.json generator cannot add, so the skin is built by a script into `skins/`:

```
SDK=../mpc-plugin-builder-sdk
python $SDK/tools/build.py .                  # once, makes build/params.json
python skin/make_backdrop.py                  # only if you change the backdrop (needs Pillow)
python skin/make_skin.py                      # after changing ui.json or the backdrop
python $SDK/tools/build.py .
$SDK/.sdk-tools/host_sim build/native/voice-morph.dylib test --wav build/test.wav
python $SDK/tools/package.py .
```

Licence: MIT. All DSP written for this plugin (YIN after de Cheveigné & Kawahara 2002; PSOLA after Moulines & Charpentier 1990).
