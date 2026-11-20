# scripts/

## build-all.sh — rebuild the flasher firmware

Compiles every Scarecrow device, merges a single flashable image per device
(offset 0), and writes them into `docs/firmware/` so the GitHub Pages web
flasher never goes stale.

```sh
./scripts/build-all.sh          # builds all 5, refreshes docs/firmware/
git add docs/ && git commit -m "refresh flasher" && git push
```

Devices built:

| Output (`docs/firmware/`) | Sketch | Chip | Panel/notes |
|---|---|---|---|
| `collector.bin`          | firmware/scarecrow            | ESP32    | huge_app |
| `cyd-28.bin`             | firmware/cyd-hud              | ESP32    | ILI9341 + `BOARD_CYD28` |
| `cyd-32.bin`             | firmware/cyd-hud              | ESP32    | ST7789 + `BOARD_CYD32` |
| `node-tembed.bin`        | firmware/df-node             | ESP32-S3 | T-Embed CC1101 |
| `node-cardputer-adv.bin` | firmware/df-node-cardputer   | ESP32-S3 | Cap CC1101 |

Override tool locations with env vars if your setup differs:
`LIBS`, `ESPTOOL`, `BOOT_APP0`.

### Toolchain

- arduino-cli with `esp32:esp32@3.3.12`
- Libraries: `ArduinoJson@7.4.3`, `RadioLib@7.1.2`, `TFT_eSPI@2.5.43`,
  `M5Cardputer@1.1.1` (pulls `M5Unified`, `M5GFX`), `ELECHOUSE_CC1101_SRC_DRV@1.0.0`,
  `ESP Async WebServer@3.13.0`, `Async TCP@3.6.1`

## CI

`.github/workflows/build-flasher.yml` runs the same script on any push that
touches `firmware/**`, then commits the refreshed bins back to `docs/firmware/`
(with `[skip ci]` so it doesn't loop). Trigger it by hand from the Actions tab
(`workflow_dispatch`) too.
