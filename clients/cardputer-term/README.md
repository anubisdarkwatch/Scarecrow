# Scarecrow Terminal — Cardputer Zero client

A keyboard-driven console for the Evil Crow **Scarecrow** (RX-only sub-GHz
sensor), built for the **M5Stack Cardputer Zero** (Raspberry Pi CM0 / Linux).
Receive-only: it reads the collector's web endpoints and sends the
tune/label/calibrate commands the firmware already exposes. No transmit path.

Zero third-party dependencies — Python 3 stdlib only (`curses`, `urllib`,
`json`) — so it installs as a tiny `.deb` and runs on a stock Pi OS image.

## Install

```sh
sudo dpkg -i scarecrow-term_0.1.0_all.deb
```

Then join the collector's Wi-Fi (`Scarecrow` / `watchonly`) and run:

```sh
scarecrow-term
```

Point it at a different host (e.g. the collector on your LAN) with:

```sh
SCARECROW_HOST=http://192.168.4.1 scarecrow-term
```

## Views & keys

| Key | Action |
|-----|--------|
| `TAB` / `1`–`4` | Switch view (LOCATE · SIGNALS · MONITOR · CONTROL) |
| `Q` | Quit |

- **LOCATE** — nodes ranked by RSSI (hottest = nearest the source), with bars.
  Shows *Standalone* when no rover is live, multi-rover DF when they are.
- **SIGNALS** — the signature library. `↑`/`↓` to pick, `L` to **label by
  typing** (the Cardputer's keyboard is the whole point), `D` to forget.
- **MONITOR** — per-band occupancy (RSSI / busy / floor).
- **CONTROL** — `←`/`→` pick a mesh band, `ENTER` to retune the whole mesh
  (hits `/fox`, which retunes the Evil Crow and broadcasts to rover nodes).

## Endpoints used (all GET, read-only firmware)

`/nodes.json` · `/sigs.json` · `/state.json` · `/log.json` · `/fox?mhz=` ·
`/label?id=&name=` · `/forget?id=`

## Packaging for the CardputerZero `.deb` repo

The tree under `pkgroot/` is a standard Debian layout; rebuild with:

```sh
dpkg-deb --root-owner-group --build pkgroot scarecrow-term_0.1.0_all.deb
```

`Architecture: all` (pure Python) installs on the Zero's arm64 image.
