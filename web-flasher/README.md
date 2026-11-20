# Scarecrow Web Flasher

A browser "click to install" page (ESP Web Tools) for the whole Scarecrow fleet:
collector, both CYD HUDs, and the T-Embed + Cardputer ADV nodes. No IDE, no
drivers — just Chrome/Edge with Web Serial.

## Layout
- `index.html` — the Darkwatch-styled flasher page
- `manifests/*.json` — one ESP Web Tools manifest per device (chip family + bin)
- `firmware/*.bin` — the merged firmware images (flash at offset 0)

## Hosting it (GitHub Pages)
ESP Web Tools needs **HTTPS** (or localhost). GitHub Pages gives you that free:

1. Push this to the repo (done — it's under `web-flasher/`).
2. On GitHub: **Settings → Pages → Build and deployment → Source: Deploy from a
   branch**, pick the branch (e.g. `main` after you merge, or `df`) and folder
   **`/ (root)`** or move `web-flasher/` to `/docs` and pick `/docs`.
3. The page will be at `https://anubisdarkwatch.github.io/scarecrow/web-flasher/`
   (or `/scarecrow/` if you host the folder at the site root).

Locally you can also test with any static server over `http://localhost`.

## Updating firmware
Rebuild a device's merged `.bin`, drop it in `firmware/` under the same name,
bump `version` in its manifest, and re-push. The page always serves the current
bins — no rebuild of the page needed.

## Chip families
- ESP32: collector, cyd-28, cyd-32
- ESP32-S3: node-tembed, node-cardputer-adv

Receive-only — no transmit path in any Scarecrow firmware.
