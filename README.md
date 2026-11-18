<div align="center">

# 🦅 SCARECROW
### receive-only sub-GHz sensor firmware for the Evil Crow RF V2
**an Anubis Darkwatch project**

</div>

Scarecrow turns the (now sell-restricted) Evil Crow RF **attack** box into a
legal passive sub-GHz **sensor**. The firmware has **no transmit path** — both
CC1101 radios are only ever placed in RX/IDLE. That's the design, not a toggle.

> **Why receive-only?** Operating a receiver needs no FCC authorization, and the
> enforcement teeth are on intentional transmitters — hardest of all on jammers.
> A binary with no TX path has nothing to misuse and nothing to certify.
> Monitoring ISM device telemetry (315 / 433 / 868 / 915 MHz remotes, TPMS,
> weather stations, LoRa/Meshtastic beacons) is lawful. Don't decode or divulge
> protected communications — ISM device chatter isn't that.

## What it does

Two radios, two concurrent jobs — the thing a single-radio tool can't do:

- **Module A — scanner.** Hops a channel plan reading RSSI (peak-hold per hop),
  tracks a per-channel noise floor, reports occupancy, and raises a
  `broadband_interference` alarm when many channels go hot at once — jammer
  **detection**, inverted from the feature that got these boards pulled.
- **Module B — listener.** Parks on one frequency, captures raw OOK burst
  **timing fingerprints** (duration + FNV hash of the pulse train). No payload
  is decoded or stored — just "a device with this signature appeared, at this
  RSSI." It also feeds the Fox Hunt instrument.

### Web dashboard (its own WiFi AP)

- **MONITOR** — self-test, channel occupancy, live event stream, jammer alarm.
- **FOX HUNT** — a live RSSI instrument: analog gauge with peak-hold, a
  WARMER/COLDER trend, an audio beacon (beeps faster + higher as you close in),
  and runtime frequency tuning. Walk toward rising dBm to find the source.

WiFi is the ESP32's own 2.4 GHz radio for the UI only; the CC1101s never leave
RX. The whole page is read-only — nothing on it can transmit or actuate.

## Hardware

Evil Crow RF V2 — ESP32-PICO-D4 + 2× CC1101 (+ NRF24, SD). Pin map in
[`firmware/scarecrow/config.h`](firmware/scarecrow/config.h), verified against
the stock V2 firmware.

## Flash it

**Browser (easiest):** open the [**web flasher**](https://anubisdarkwatch.github.io/Scarecrow/)
in Chrome / Edge / Opera, plug in by USB, click flash.

**esptool:**
```
esptool.py -c esp32 -p <PORT> write_flash 0x0 docs/scarecrow.merged.bin
```

Then join WiFi **`Scarecrow`** / **`watchonly`** → browse to **http://192.168.4.1**.
Change the SSID/password in `config.h` before deploying.

## Build from source

Board: **ESP32 Dev Module**, 4 MB, `huge_app` partition. Dependencies — all
vendored or cloned, none from the Arduino registry (the registry
ESPAsyncWebServer is broken on ESP-IDF 5.x):

- `ELECHOUSE_CC1101_SRC_DRV` — dual-module fork, vendored in `firmware/libraries/`
- `ESP32Async/AsyncTCP` + `ESP32Async/ESPAsyncWebServer` — clone into your libs path

```bash
arduino-cli core install esp32:esp32 \
  --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
git clone https://github.com/ESP32Async/AsyncTCP          libs/AsyncTCP
git clone https://github.com/ESP32Async/ESPAsyncWebServer libs/ESPAsyncWebServer
arduino-cli compile -b esp32:esp32:esp32:PartitionScheme=huge_app,FlashSize=4M \
  --libraries firmware/libraries,libs firmware/scarecrow
```

CI does exactly this — see [`.github/workflows/release.yml`](.github/workflows/release.yml).
Push a `v*` tag and it builds the firmware, publishes the web flasher to GitHub
Pages, and attaches the binaries to a Release.

## Output schema — NDJSON on Serial @115200 (+ optional SD)

```
{"t":"boot","fw":"scarecrow-0.7","tx":false,...}
{"t":"selftest","mod":"A","version":20,"marcstate":13,"in_rx":true,"rssi_raw":212}
{"t":"busy","mhz":433.92,"rssi":-62,"floor":-104}
{"t":"burst","mhz":433.92,"edges":132,"dur_us":41880,"sig":"8f2a1c","rssi":-58}
{"t":"alarm","kind":"broadband_interference","hot":6,"peak_mhz":915.00,"peak_rssi":-41}
```
(`fox` events — fast live RSSI — are pushed to the web UI only, not the serial log.)

## Roadmap

- [x] Receive-only core, dual-radio, web dashboard
- [x] Fox Hunt RSSI instrument
- [x] Signature library — persist/label burst sigs, flag first-seen emitters
- [ ] Multi-node direction finding (bearing via RSSI delta / multilateration)
- [ ] Meshtastic reporting path

## Credits

Built on the [Evil Crow RF V2](https://github.com/joelsernamoreno/EvilCrowRF-V2)
hardware and its dual-module `ELECHOUSE_CC1101_SRC_DRV` driver (vendored,
retains its upstream license). Async web stack by
[ESP32Async](https://github.com/ESP32Async).

## License

Firmware and docs: **CC BY 4.0** (matches upstream Evil Crow). Vendored code in
`firmware/libraries/` retains its original license. See [`LICENSE`](LICENSE).

> Scarecrow is for lawful monitoring, RF research, and spectrum hygiene on bands
> you're permitted to receive. You are responsible for complying with local law.

## Direction finding (experimental — `df` branch)

Multi-node RSSI localisation over **ESP-NOW**. The Evil Crow is the **anchor +
collector** (runs the web UI, hosts the `LOCATE` view); one or more **rover
nodes** (any ESP32/ESP32-S3 + a single CC1101) park on the target frequency and
broadcast their RSSI + burst `te` to the anchor. Hottest node = nearest the
source. No TDOA (CC1101 can't) — this is coarse RSSI ranking, not a pinpoint.

- **Node firmware:** `firmware/df-node/` — set your board in `node_config.h`
  (`BOARD_STICKS3_RFPACK` with verified pins, or `BOARD_GENERIC_CC1101` for a
  Rabbit Labs Flux Capacitor / Slim-Line / bare EByte CC1101 you wire yourself),
  a unique `NODE_ID`, and the channel to match the anchor's `AP_CHANNEL`.
- **Collector:** enabled by `USE_DF` in `firmware/scarecrow/config.h`; receives
  ESP-NOW reports and ranks nodes in the `LOCATE` tab. The Evil Crow is node `0`.
- **Caveats:** ESP-NOW peers must share the anchor's WiFi channel. Amplified
  nodes (Flux Capacitor LNA) read systematically hotter — calibrate a per-node
  offset before trusting the ranking across mixed radios; a matched pair is cleanest.
