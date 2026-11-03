# Emotion LED Matrix

A speech-reactive LED media art piece. You speak into a laptop microphone, a Korean emotion classifier picks one of 60 emotions, and a 70 × 18 LED matrix responds with a wave whose color and motion match that emotion.

Solo project, Winter 2025.

## How it works

```
Laptop microphone (5 s clip)
  → faster-whisper (Korean speech-to-text)
  → hun3359/klue-bert-base-sentiment (60-class emotion classifier)
  → emotion → RGB color (60-entry palette) + wave style (6 families)
  → USB serial "R,G,B,S"
  → ESP32 → 70 × 18 WS2812B matrix (1,260 LEDs)
```

The host computer handles all inference. The ESP32 only receives a color and a style index and renders the animation.

## Repository layout

| Path | Contents |
|---|---|
| `host/main.py` | Records audio, runs speech-to-text and emotion classification, sends commands to the ESP32 |
| `host/emotion_colors.py` | 60-emotion RGB palette, grouped into six emotion families |
| `firmware/emotion_led_matrix/` | ESP32 firmware (Arduino, FastLED) |
| `tools/convert_video.py` | Converts any video into 1-bit 70 × 18 frames packed into a PROGMEM header |

## Emotion → animation mapping

The classifier's 60 labels (AI Hub emotional dialogue corpus: 6 families × 10 sub-emotions) each get their own color. The family decides the motion style:

| Style | Family | Character of the wave |
|---|---|---|
| 0 | Anger | Fast, short tail |
| 1 | Sadness | Very slow, long trailing tail |
| 2 | Anxiety | Fast, high vertical frequency (jittery) |
| 3 | Hurt | Slow, long tail |
| 4 | Embarrassment | Medium speed (also the fallback) |
| 5 | Joy | Three wave heads in a row |

Colors are grouped by family (anger = red/orange, sadness = blue/indigo, anxiety = yellow/amber, hurt = magenta/purple, embarrassment = pink/lavender, joy = gold/warm). Within a family, brightness and saturation vary with the intensity of the sub-emotion.

## Firmware behavior

- **Idle background:** a slowly drifting spring palette (greens and yellow-greens) made from two layered sine fields.
- **Wave:** each emotion sends a wave sweeping left to right across the matrix in that style.
- **Afterimage:** every column the wave passes keeps a tint of the emotion color (up to 60 %), fading linearly over 30 s. The background behind the wave also shifts to a new random phase.
- **Crossfade:** if a new emotion arrives while a tint is still visible, the tint color crossfades over 800 ms.
- Frame time is 16 ms (about 60 fps). Brightness is capped at 50/255.

## Serial protocol (115200 baud, newline-terminated)

| Command | Effect | Reply |
|---|---|---|
| `R,G,B,S` | Start a wave with color (R,G,B) and style S (0–5) | `ACK R,G,B,SS` |
| `TEST,S` | Test wave in red with style S | `ACK …` |
| `STOP` | Clear all afterimages and return to idle | `ACK STOP` |
| `BADAPPLE` | Play the bitmap animation, if the frame data was compiled in | `ACK BADAPPLE` / `ERR BADAPPLE NOT BUILT` |

On boot the ESP32 prints `READY`. The host scans the serial ports and connects to the first one that answers `READY`.

## Hardware

- ESP32 dev board
- 70 × 18 WS2812B matrix, data on GPIO 13, wired as a row-wise serpentine (even rows left → right, odd rows right → left)
- `COLOR_ORDER` is `GRB` for this strip. If colors look wrong, check the comment at the top of the firmware.

## Setup

**Firmware** (Arduino IDE or arduino-cli, ESP32 board package, FastLED library):

Open `firmware/emotion_led_matrix/emotion_led_matrix.ino` and upload it to the ESP32.

The `BADAPPLE` mode plays a video from a generated `bad_apple_frames.h`. The header is not included in this repository, because the frame data is derived from a third-party video. Without it, the firmware still builds and that one command is disabled. To generate it from a video file you have:

```bash
pip install opencv-python numpy
python tools/convert_video.py your_video.mp4
# move the generated bad_apple_frames.h into firmware/emotion_led_matrix/
```

The converter resizes each frame to 70 × 18, thresholds it to black and white, and packs it at 1 bit per pixel (158 bytes per frame). At 20 fps, a 3.5-minute video comes to about 3,300 frames, roughly 510 KB of flash.

**Host** (Python 3.9+):

```bash
cd host
pip install -r requirements.txt
python main.py
```

Press Enter to record a 5-second clip, or `q` to quit. The first run downloads the Whisper `base` model and the classifier from Hugging Face.

## Models

- Speech-to-text: [faster-whisper](https://github.com/SYSTRAN/faster-whisper) `base`, CPU, int8
- Emotion: [hun3359/klue-bert-base-sentiment](https://huggingface.co/hun3359/klue-bert-base-sentiment)
