# DQ-DSP — ESP32-S3 Firmware

### 🎥 [**Watch the demo**](https://youtu.be/UXzky9iujUc)  ·  📝 [**Full write-up & build story**](https://tamduongs.com/blog/dq-dsp)  ·  👉 [**Live UI**](https://dq-dsp.tamduongs.com)  ·  📦 [**Pre-built v1.0.0 binary**](https://github.com/agooddaytowork/dq-dsp-firmware/releases/tag/v1.0.0)  ·  🖥 [**UI source**](https://github.com/agooddaytowork/dq-dsp-ui)

USB Audio Class 1.0 → on-device DSP → dual I2S out for two PCM5102A
breakouts. Pairs with the **DQ-DSP web UI** for live parameter control
over a second USB cable (UART0 over the DevKitC-1's USB-Serial-JTAG port).

> *(DQ = my daughter's name. Yes, I named a DSP after her. Don't @ me.)*

> **📝 [The full story →](https://tamduongs.com/blog/dq-dsp)** —
> *why an active speaker, what I tried first (a Wondom ADAU1701 board, a
> failed attempt to ship a miniDSP into Vietnam), and the complete $20
> build journey with photos. This README is the technical reference;
> the blog is the narrative.*

[![DQ-DSP UI](docs/images/dq-dsp-ui.png)](https://dq-dsp.tamduongs.com)

### ▶ Video demo

[![Watch the 3-minute demo on YouTube](https://img.youtube.com/vi/UXzky9iujUc/maxresdefault.jpg)](https://youtu.be/UXzky9iujUc "Click to watch on YouTube")

*Bench tour, browser UI, and a bi-amped bookshelf speaker actually playing music.*

## 🆕 Hi-res upgrade — 24-bit / 48 kHz

The USB Audio Class endpoint now streams **24-bit / 48 kHz stereo** end-to-end.
The whole path — TinyUSB ring buffer, DSP pipeline, dual I²S TX — was
widened to a 32-bit slot with 24 valid bits (MSB-aligned), so PCM5102A latches
the upper 24 bits directly. That's **+48 dB of dynamic-range headroom** over
the previous 16-bit build with zero compatibility risk on macOS / Windows /
Linux UAC 1.0 hosts.

![macOS Audio MIDI Setup showing usb uac at 48.000 Hz 2 ch 24-bit Integer](docs/images/24bit-48khz.png)

*macOS Audio MIDI Setup confirming the device negotiates 48 kHz, 2 channel,
24-bit Integer.*

## Audio path

```
USB host  →  TinyUSB UAC  →  application ring buffer (24 ms cushion)
          →  DSP pipeline (Core 1):
              input gain · phase · mute
              10-band Room EQ ×2 ch
              10-band Input PEQ ×2 ch
              2 × 4 routing matrix
              10-band Output PEQ ×4 ch
              crossover (HP + LP, LR / Butterworth, 6/12/18/24 dB/oct)
              gain · delay (0–10 ms) · phase · mute
          →  Dual I2S TX  →  2 × PCM5102A  →  4 line outs
```

Control frames (SLIP + CRC-8) for the web UI's live parameter streaming,
bulk Apply, and NVS Save-to-Device travel over **UART0** — exposed on the
DevKitC-1's **USB-Serial-JTAG** port (the same one you flash from). Audio
and control are on **two separate USB-C cables** plugged into the two
ports of the dev-board:

| DevKitC-1 port           | Used for                               | Cable |
|--------------------------|----------------------------------------|-------|
| USB-Serial-JTAG (UART0)  | flashing + Web Serial parameter control| #1    |
| Native USB-OTG           | USB Audio Class (UAC) — the actual audio stream | #2 |

## DSP specs

### Audio I/O

| | |
|---|---|
| USB input | UAC 1.0, 2 channels, **24-bit / 48 kHz** |
| Analog output | 4 channels (2 × stereo line) via 2 × PCM5102A I²S DACs |
| DAC dynamic range | 112 dB (PCM5102A datasheet, A-weighted) |
| Output drive | 2.1 Vrms typical line level, AC-coupled — feed any line-level amp / powered speaker |

### DSP pipeline (per audio block)

| | |
|---|---|
| Internal precision | 32-bit float |
| Sample rate | 48 kHz fixed (matches USB UAC + I²S) |
| Block size | 128 samples (~ 2.7 ms) |
| **Per-input (×2)** | 10-band Room EQ + 10-band Input PEQ + gain · phase · mute |
| **Routing** | 2 in × 4 out matrix · per-cell enable + linear gain (0–100 %) |
| **Per-output (×4)** | 10-band PEQ + crossover (HP + LP) + delay + gain · phase · mute |
| PEQ filter types | peak · low / high shelf · low / high pass · notch |
| PEQ ranges | freq 20 Hz – 20 kHz · gain ±15 dB · Q 0.1 – 30 |
| Crossover topologies | Linkwitz-Riley, Butterworth |
| Crossover slopes | 6 / 12 / 18 / 24 dB/oct |
| Delay range | 0 – 10 ms per output, sample-accurate |
| Gain range | −72 to +12 dB per channel + post-processing master |
| Limiter | Soft-clip on master output |

### Real-time behaviour

| | |
|---|---|
| Buffering latency | 24 ms application cushion + up to 60 ms I²S DMA at 48 kHz; host latency additional (not measured end-to-end) |
| Clock recovery | Adaptive USB feedback, ±200 ppm maximum; one output frame per input frame, no local ASRC |
| Telemetry rate | 1 Hz over USB CDC (CPU load, drift PPM, buffer fill) |
| DSP load | ~30–40 % of one Core (Core 1 pinned, 240 MHz) at full pipeline |
| Atomic config swap | shadow-buffer commit between audio blocks — no clicks on parameter changes |

### USB clock recovery (issue #2)

The USB host and I²S output clocks are independent. A single controller watches
our I²S-paced application ring and adjusts the asynchronous USB feedback endpoint:
positive PPM requests more samples from the host, negative PPM requests fewer.
TinyUSB's separate FIFO-count controller is disabled. DSP processes each incoming
stereo frame exactly once, without local interpolation or sample-rate changes.
This ports the TAC5212 adaptive-feedback fix (`d7ba2d4`) and synchronous speaker
buffer ownership fix (`63eeeb7`) from the DSP repository. The receiver also drains
extra host frames immediately, so positive feedback reaches the application ring.

Existing presets and serial commands retain their binary layout. The System panel's
legacy fields now mean:

- **Kp:** feedback proportional gain in ppm/ms; default 25, accepted range 5–60.
- **Ki:** feedback integral gain in ppm/(ms·s); default 0.5, accepted range 0.05–2.
- **Target Fill:** retained for compatibility, ignored; the cushion is fixed at
  24 ms (about 3.5% of the 192 KiB ring at 24-bit/48 kHz).
- **Max PPM:** clamped to 20–200; zero/unset uses 200.

Out-of-range Kp/Ki values from older presets use the new defaults. The controller
filters fill measurements, limits its slew to 2 ppm per 100 ms, prevents integral
wind-up, and suspends integration during pause or priming. Playback primes at
24 ms and returns to silence/refill below 12 ms. Telemetry reports host feedback
PPM, whose sign is the opposite of the old ASRC consumption correction.

Run `python3 tests/test_usb_feedback.py` for host-side C regression checks covering
30-minute drift simulations, pause/resume, saturation, USB extra/partial frames,
backlog draining, and one-for-one PCM processing. Build with `./idf.sh build`.
Before a release, verify sustained playback on hardware (at least 30 minutes),
pause/resume and USB reconnect, on the intended hosts. Host tests cannot verify
USB host feedback behavior, DMA timing, or audible output.

### Hardware platform

| | |
|---|---|
| MCU | ESP32-S3 N16R8 — Xtensa LX7 dual core @ 240 MHz |
| Flash | 16 MB QSPI |
| PSRAM | 8 MB Octal |
| USB | 2 × USB-C: USB-Serial-JTAG (UART0) + native USB-OTG |
| ESP-IDF | v5.2 |

### Control link

| | |
|---|---|
| Transport | USB CDC-ACM via UART0 / USB-Serial-JTAG bridge |
| Baud rate | 115 200 8N1 |
| Frame format | `0xAA 0x55 \| length \| payload \| CRC-8 (poly 0x07)` |
| Param updates | live diff streaming, ~5 ms typical RTT (browser → device → ack) |
| Bulk push | "Apply" — entire config in one frame burst |
| Persistence | NVS commit on "Save to Device" (explicit user action; deferred to avoid audio glitch from flash-erase blocking the audio task) |

## What do I actually do with this?

Plug an ESP32-S3 dev-board with two PCM5102A breakouts soldered in into
your laptop. Your OS sees a generic USB audio device showing up as
**usb uac** in the audio picker (that's the UAC interface descriptor
the upstream TinyUSB component ships — feel free to rename it to
*DQ-DSP* in your OS sound settings if you want). Open the web UI in
Chrome on the same laptop, click **Connect Serial**, and now every PEQ
knob you drag goes straight into the chip — no recompile, no MIDI, no
$400 miniDSP.

![How DQ-DSP plugs into your system](docs/images/usage-diagram.svg)

### The use case I actually built it for: active 2-way speakers

Bookshelf speakers are usually passive — the crossover is a chunk of
inductors and capacitors inside the cab, splitting one full-range
amplifier feed into woofer + tweeter. That passive XO has tradeoffs:
phase shift, insertion loss, no per-driver EQ, no time alignment. In
Vietnam it's also painful to source — decent air-core inductors and
audiophile-grade film caps cost more than the drivers themselves.

**Active bi-amp** rips the passive crossover out and feeds each driver
its own amplifier, with the crossover done in DSP. DQ-DSP makes that a
$20 BOM:

| DSP output | Drives           | Typical settings                  |
|------------|------------------|-----------------------------------|
| Out 1      | Left woofer      | LP @ XO freq, LR4 24 dB/oct       |
| Out 2      | Left tweeter     | HP @ XO freq, LR4 24 dB/oct, +delay if needed |
| Out 3      | Right tweeter    | mirror of Out 2 (link group)      |
| Out 4      | Right woofer     | mirror of Out 1 (link group)      |

You pick the crossover frequency from the driver's spec sheet (usually
1.5–3 kHz for a typical 1" dome / 5" mid). Add per-driver gain trim,
delay for time-alignment, and PEQ for any baffle / room peaks. Save the
preset, commit to flash, unplug from the laptop — the box keeps playing
forever from any USB host.

**Reference build** — the speakers I actually drive with this thing:

| Role     | Driver                        | Notes                                                            |
|----------|-------------------------------|------------------------------------------------------------------|
| Tweeter  | **Dayton Audio NHP25Ti-4**    | 1" titanium dome, 4Ω. ~$28. Crosses over comfortably from ~2 kHz. |
| Mid/Bass | **Dayton Audio TCP115-4**     | 5" paper-cone polypropylene-coated, 4Ω. ~$38. Tight bottom.       |

Dayton drivers are cheap and well-measured — Parts Express publishes
proper FRD/ZMA files, so you can simulate the crossover in VituixCAD or
similar before cutting wood. Pictures of the actual cabs and the
breadboarded DSP are in the [blog post](https://tamduongs.com/blog/dq-dsp).

## Bill of materials

| # | Part | Qty | Notes |
|---|------|-----|-------|
| 1 | **ESP32-S3-DevKitC-1 N16R8** | 1 | 16 MB flash + 8 MB Octal PSRAM. Standard 38-pin Espressif dev-board (also sold by Waveshare, AliExpress clones, etc.). |
| 2 | **GY-PCM5102 / TENSTAR ROBOT PCM5102A** breakout | 2 | Purple board with 3.5 mm jack on the long edge and L/R/G analog pads. ~$2 each on AliExpress. |
| 3 | USB-C data cable | 2 | DevKitC-1 has two USB-C ports — **USB-Serial-JTAG** (UART0, used for flashing AND web-UI control) and **native USB-OTG** (used for UAC audio). Both stay plugged in during normal use. *Both must be data-capable, not power-only.* |
| 4 | Dupont jumper wires (M-F) | ≥ 12 | 3 I2S signals × 2 DACs + shared 3V3 + GND = 8 minimum, plus a couple spares for the jumper pads. |
| 5 | 3.5 mm audio cable / pigtail | 2 | One per DAC, into your amp or powered speakers. |

Total parts cost: roughly **US $15–20** at the time of writing. ESP-IDF
v5.2 toolchain is the only software dependency.

## Wiring

![Wiring diagram — ESP32-S3 → 2× PCM5102A](docs/images/pin-diagram.svg)

| Function           | ESP32-S3 GPIO |
|--------------------|---------------|
| I2S0 BCK (DAC #1)  | 4             |
| I2S0 LRCK          | 5             |
| I2S0 DOUT          | 6             |
| I2S1 BCK (DAC #2)  | 16            |
| I2S1 LRCK          | 17            |
| I2S1 DOUT          | 18            |

Both DACs share **3V3** + **GND**. On each PCM5102A breakout:
- **XSMT** → 3V3 (un-mute — default jumper position is LOW = silence)
- **SCK** → GND (chip generates MCLK internally from BCK)
- FLT / DEMP / FMT → default LOW

Pin assignments are overridable via `idf.py menuconfig` →
*Audio DSP I2S Configuration (ESP32-S3)*.

## Flash a pre-built binary

Grab the merged image from the
[v1.0.0 release](https://github.com/agooddaytowork/dq-dsp-firmware/releases/tag/v1.0.0)
(the ``dq-dsp-firmware-1.0.0.bin`` asset — single file, flashed at offset
``0x0``).

### 1. Install esptool

    pip install esptool                # any Python 3.8+ env

### 2. Find the serial port

The DevKitC-1 has two USB-C connectors. Either one works for flashing —
the chip's ROM exposes a USB-Serial-JTAG / CDC interface in download
mode regardless of which port you plug in. Connect at least one cable
and look for a new device:

| OS         | Command                                                |
|------------|--------------------------------------------------------|
| **macOS**  | ``ls /dev/cu.usbmodem*`` — pick the new entry          |
| **Linux**  | ``ls /dev/ttyACM* /dev/ttyUSB* 2>/dev/null`` (or watch ``dmesg | tail`` after plug-in) |
| **Windows**| Device Manager → *Ports (COM & LPT)* → look for **USB Serial Device (COMxx)** |

> 💡 If two ports show up, either is fine for flashing — pick whichever
> is new. The other one will host the audio stream once the firmware is
> running.

### 3. Quit anything that's holding the port

If the chip is already running an earlier build of the firmware:

- macOS Sound Settings: switch the audio output **away** from "usb uac"
  (or whatever you renamed it to)
- Web UI: if you're connected, click **Disconnect**
- Other terminal apps (Arduino IDE serial monitor, ``screen``,
  ``picocom``) — close them

If ``esptool`` complains **"Resource busy"**, this is why.

### 4. Flash

    python -m esptool --chip esp32s3 \
      --port /dev/cu.usbmodemXXXX \
      -b 460800 \
      --before default_reset --after hard_reset \
      write_flash 0x0 dq-dsp-firmware-1.0.0.bin

Replace ``/dev/cu.usbmodemXXXX`` with whatever step 2 turned up
(on Linux it's ``/dev/ttyACM0``-ish, on Windows ``COM5`` or similar).
You should see ``Hash of data verified.`` followed by ``Hard resetting
via RTS pin...``.

After reset the chip enumerates as **usb uac** in your OS audio picker
(rename it to *DQ-DSP* in your sound settings — macOS, Windows, and
Linux all let you do this; the rename sticks per host).
[Open the UI](https://dq-dsp.tamduongs.com) in Chrome, click
**Connect Serial**, pick the device's `usbmodem*` port, and start tweaking.

## Build from source

ESP-IDF v5.x required. Set up per
<https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/>.

    . $IDF_PATH/export.sh
    idf.py set-target esp32s3
    idf.py build
    idf.py flash monitor

The included `./idf.sh` is a convenience wrapper.

`sdkconfig.defaults` pins the project-specific Kconfig values; the
generated `sdkconfig` is gitignored (user-local).

## Repository layout

    .
    ├── main/               # ESP-IDF component
    │   ├── main.c          # app_main: init NVS / DSP / I2S / USB
    │   ├── usb_audio.c     # TinyUSB UAC + CDC, adaptive USB feedback
    │   ├── i2s_audio.c     # Dual I2S TX driver
    │   ├── serial_server.c # CDC frame parser → dsp_param_apply
    │   ├── Kconfig.projbuild
    │   └── CMakeLists.txt
    ├── shared/dsp/         # Pure-C DSP core
    │   ├── dsp_config.h
    │   ├── dsp_pipeline.c  # Biquad chain, routing matrix, crossover
    │   ├── dsp_param_update.c   # Atomic shadow-buffer swap
    │   ├── msg_handler.c   # Wire-protocol dispatch
    │   ├── serial_protocol.h    # Canonical frame layout (mirrored in UI repo)
    │   └── ble_protocol.h  # Header-compat constants (BLE transport removed at runtime)
    ├── CMakeLists.txt
    ├── partitions.csv
    ├── sdkconfig.defaults
    └── dependencies.lock

## Wire protocol

`shared/dsp/serial_protocol.h` is the canonical definition. Frame layout:

    0xAA 0x55 | length (1 byte) | payload | CRC-8 (poly 0x07)

The UI's TypeScript decoder lives at
[`src/types/serial-protocol.ts`](https://github.com/agooddaytowork/dq-dsp-ui/blob/main/src/types/serial-protocol.ts)
in the companion repo and is a one-to-one mirror.

## Web UI companion

UI source: <https://github.com/agooddaytowork/dq-dsp-ui>. Live deploy:
<https://dq-dsp.tamduongs.com>. Connect via Web Serial on Chrome / Edge /
Brave / Opera desktop — the UI auto-detects unsupported browsers
(Firefox, Safari, mobile) and shows a banner.

## Support the project

If DQ-DSP is useful to you and you want to chip in for hardware
prototypes, parts, or coffee while the next firmware version cooks,
buy me a Ko-fi:

[![ko-fi](https://img.shields.io/badge/Support%20on-Ko--fi-FF5E5B?logo=ko-fi&logoColor=white)](https://ko-fi.com/tamdnq)

Anything is appreciated and goes straight into the build.

## License

[GPLv3](./LICENSE). ESP-IDF and its components are Apache 2.0; combined
binaries inherit GPL terms (one-way compatible). Forks and derivatives
of this firmware must remain open-source under the same license.

Copyright © 2026 Tam Duong (<https://tamduongs.com/>).
