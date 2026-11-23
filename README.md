# Alien Displays

Wi-Fi message displays in 3D-printed grey-alien heads. Each one runs animated eyes,
a clock, and messages sent from the family web app, from anywhere.

## Layout

| Folder | What |
|---|---|
| `firmware/alien_v2/` | The firmware every display runs (ESP32-C3 SuperMini + MAX7219 FC-16 4-in-1) |
| `firmware/legacy/` | Earlier sketches: first-light test, eyes only, v1 local-Wi-Fi version |
| `app/` | The web app (single static page, deployed by Netlify) |
| `supabase/migrations/` | Database schema, in the order applied to the live project |
| `hardware/` | Notes on the printed parts |
| `.github/workflows/` | Builds firmware and uploads it as a test release |
| `tools/emoji.py` | Pixel emoji designs; run it to regenerate `firmware/alien_v2/emoji_glyphs.h` |

## Hardware

- ESP32-C3 SuperMini, mounted on end so its USB-C exits the back lid
- MAX7219 FC-16 4-in-1 red dot matrix (32 x 8)
- Wiring: DIN to GPIO 6, CLK to GPIO 4, CS to GPIO 7, VCC to 5V, GND to G
- Power: USB-A charger and A-to-C cable (some SuperMini clones won't take C-to-C power)

## Building firmware by hand (Arduino IDE)

- Board **ESP32C3 Dev Module**, **USB CDC On Boot: Enabled**
- Partition Scheme **Minimal SPIFFS (1.9MB APP with OTA)**. The default is too small,
  and cloud releases must use this scheme too
- Libraries: MD_Parola, MD_MAX72XX, WiFiManager (tzapu), ArduinoJson
- Copy `firmware/alien_v2/secrets.example.h` to `secrets.h` and set the Wi-Fi upload password

## Releasing firmware

1. Bump `FW_VERSION` in `alien_v2.ino` (e.g. `2.2.0`) and commit to `main`.
2. In GitHub: **Actions → Firmware → Run workflow** (or publish a GitHub release tagged `v2.2.0`).
3. GitHub builds it and uploads it as an **unpublished test release**.
4. In the web app, **Publish** it. Displays set to *Test releases* install it within ~15 s.
5. Once happy, set the release to everyone (or upload again with channel *stable*).

The tag must match `FW_VERSION`, or the build stops. Otherwise displays
would reinstall the same release in a loop.

## How displays talk to the cloud

Displays never log in as people. On first boot a display calls `device_register`
with its chip ID and gets a secret token (only its hash is stored) plus a 6-character
claim code, which it scrolls until someone adds it in the app (`claim_device`).
From then on it calls `device_poll` every 15 s to check in, collect messages and
settings, and learn about new firmware.

## Known gaps

- Firmware downloads are SHA-256 verified, but TLS certificates aren't checked yet (`setInsecure`).
- No automatic rollback yet (the Arduino build doesn't enable the rollback bootloader).
- Replayed messages are kept in memory only, so they clear on restart.
