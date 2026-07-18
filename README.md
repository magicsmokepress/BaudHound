# 🐾 BaudHound — Wireless Com Terminal

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

A pocket-sized **wireless serial console** for the ESP32-S3. Tap any device's UART and read/write it over **WiFi (web + telnet)** *or* over **USB**, all at once — with **automatic baud-rate detection**, so you don't have to know the port speed in advance.

Built for bench work: probe a device's TTL serial port, and BaudHound hunts for the baud, streams the output to your phone/laptop, and lets you type back.

## Why

**Reprogram and poke at your serial TTL devices on the go.** Clip onto a TTL port and drive it from the phone already in your pocket — no dragging a serial cable across the rack, no lugging a laptop over to the header. Useful for the switch in the closet, the sensor node on a pole, the board wedged behind a panel.

> Not intended as a permanent TCP/IP-to-TTL serial gateway. It's a hop-on/hop-off bench tool — the open AP and unauthenticated console are fine for a quick session, not for leaving wired into anything long-term. For a permanent bridge, use a hardened, authenticated device.

---

## Features

- **Three ways in, simultaneously**
  - **Web UI** — open a page, watch live output, type commands, change baud from a dropdown.
  - **Telnet** (port 23) — connect with PuTTY / `nc` / any telnet client (up to 4 at once).
  - **USB serial** — the board shows up as a COM port; raw, character-by-character passthrough (great for interactive menus).
- **Automatic baud detection** — multi-pass scan across standard rates (9600 → 921600, incl. 14400/28800/74880). Locks on to whatever the target is emitting.
- **Active "wake" probe** — optionally sends an Enter to nudge a silent console into responding, then detects.
- **RX/TX auto-swap** — reversed the data wires? It flips orientation and recovers on its own.
- **Captive-portal setup** — join the open AP, a page pops up; from there you can join your own WiFi.
- **Status LEDs** — onboard RGB shows network state; optional external RGB shows link state.

---

## Hardware

| Item | Detail |
|------|--------|
| Board | **Teyleten ESP32-S3-N16R2** (ESP32-S3, 16 MB flash, 2 MB QSPI PSRAM) — or any ESP32-S3 dev board |
| Target link | UART on **GPIO16 (RX)** / **GPIO17 (TX)**, **GND** |
| Onboard status LED | WS2812 on **GPIO48** |
| Optional external RGB | common-cathode: **R→37, G→39, B→40**, common → **GND** |
| Power | USB, or 5 V |

### Wiring to the target device
```
BaudHound GPIO16 (RX)  <---- target TX      (read the target's output)
BaudHound GPIO17 (TX)  ----> target RX      (send to the target; omit for read-only)
BaudHound GND          <---> target GND     (required)
```
- **3.3 V logic.** For a 5 V target, add a level shifter / divider on its TX line.
- **Read-only / "data-diode":** just wire target-TX → GPIO16 and GND. Leave GPIO17 disconnected.

> ⚠️ Common ground is mandatory. If the two boards are powered from different supplies, the GND wire is what makes the link work.

---

## Build & Flash (Arduino IDE)

1. **Install the ESP32 core** (Boards Manager → *esp32* by Espressif, 3.x).
2. **Install libraries** (Library Manager):
   - **Async TCP** by *ESP32Async*
   - **ESP Async WebServer** by *ESP32Async*
   > Use the **ESP32Async** forks. The older `me-no-dev` versions fail to compile on core 3.x.
3. **Board settings** (Tools menu):

   | Setting | Value |
   |---------|-------|
   | Board | **ESP32S3 Dev Module** |
   | PSRAM | **QSPI PSRAM** |
   | Flash Size | **16MB (128Mb)** |
   | USB CDC On Boot | **Enabled** |
   | Upload Mode | UART0 / Hardware CDC |

4. Open `firmware/BaudHound/BaudHound.ino`, select your port, **Upload**.
5. Flash the test target (optional): `firmware/BaudHound_Companion/BaudHound_Companion.ino` on any ESP32-WROOM.

> **After uploading, press the RESET/EN button once** if the board sits at "waiting for download" — see [Troubleshooting](#troubleshooting).

---

## First run

1. On boot the device starts an **open WiFi AP** named **`BaudHound-XXXX`** (XXXX = last MAC bytes).
2. Join it from your phone/laptop. A **captive page** should pop up; if not, browse to **`http://192.168.4.1/`**.
3. That's the console. To put it on your own network instead, tap **⚙ Settings → WiFi → Scan → pick your SSID → Save & Join**. The AP stays up as a fallback, and you can then also reach it at **`http://baudhound.local/`**.

---

## Using it

### 1. Web console
Open `http://192.168.4.1/` (on the AP) or `http://baudhound.local/` (on your LAN).
- Live output streams into the log.
- Type in the box + **Enter** to send (↑/↓ recalls history; commands echo as `> ...`).
- **Baud dropdown** in the top bar: pick **Auto** or a fixed speed.
- Tap the command **chips** (`~baud`, `~probe`, …) to insert them.

### 2. Telnet with **PuTTY**  ⭐
1. Find the IP: `192.168.4.1` on the AP, or the LAN IP shown in **⚙ Settings** after joining WiFi.
2. Open **PuTTY** and set:
   - **Host Name (or IP address):** `192.168.4.1` (or the LAN IP)
   - **Port:** `23`
   - **Connection type:** **Telnet**
3. Click **Open**. You'll see `[connected]` and the target's stream. Type to send.

**Tips**
- To see what you type, enable **Terminal → Local echo: Force on**.
- For clean line endings, **Terminal → Implicit CR in every LF** can help with some targets.
- Up to **4 telnet clients** can watch at once.

Command-line equivalents:
```bash
telnet 192.168.4.1 23        # classic telnet
nc 192.168.4.1 23            # netcat
```

### 3. USB serial (PuTTY or any terminal)
Plug BaudHound into your PC — it enumerates as a **COM port**.
- **PuTTY:** Connection type **Serial**, **Serial line** = your COM port, **Speed** = anything (native USB-CDC ignores it), **Open**.
- This path is **raw / per-character**, so interactive menus, arrow keys, and prompts-without-newlines all work.
- The **actual target baud** is whatever BaudHound is set to (Auto or fixed) — set it in the web UI, not in PuTTY.

---

## Commands

Type these in the **web** or **telnet** console (not the USB passthrough — there they'd be sent to the target).

| Command | Action |
|---------|--------|
| `~baud` | Auto-detect the target's baud now |
| `~baud <n>` | Pin a fixed baud (e.g. `~baud 115200`) — stops auto-repolling |
| `~baud auto` | Return to auto-detect mode |
| `~probe` | Toggle **active wake-probe** (sends Enter during detection) |
| `~probe on` / `~probe off` | Force wake-probe on/off |
| `~swap` | Swap RX/TX pin assignment (for a reversed cable) |
| `~version` / `~info` | Show firmware version + current link settings |
| `~ledtest` | Cycle the status LEDs to verify wiring/colors |

Anything **without** a leading `~` is sent to the target device as-is.

---

## Baud auto-detection

BaudHound listens on each candidate speed over several short passes and locks on to the one that yields clean, printable text. It handles **bursty/periodic** output (e.g. one line per second) by accumulating samples across passes.

- **Continuous data** → locks almost instantly.
- **Silent console** → enable **wake-probe** (`~probe on`), which sends an Enter to elicit a prompt.
- **Undetectable stream** (binary data, or a non-standard baud) → after a few tries it pauses and asks you to pin a baud. A binary port simply has no "text" to show.
- **Known speed?** Pin it with the dropdown or `~baud <n>` — most reliable, and disables the re-scan churn.

Scanned rates: `9600, 14400, 19200, 28800, 38400, 57600, 74880, 115200, 230400, 460800, 921600`.

---

## Status LEDs

**Onboard WS2812 (GPIO48) — network status**

| Color | Meaning |
|-------|---------|
| 🟢 Green | AP up, not joined to WiFi |
| 🔵 Blue | Joined to a WiFi network |
| 🔴 Red (flash) | Serial data on the wire (RX/TX) |

**Optional external RGB (37/39/40) — link status**

| Color | Meaning |
|-------|---------|
| 🟢 Green blink | Scanning / probing for baud |
| 🔵 Blue | Locked & receiving |
| 🔴 Red | RX/TX orientation swapped |
| ⚫ Off | Idle |

Run `~ledtest` to confirm colors; if any are wrong on your wiring, it's a one-line change in the defines.

---

## Companion test target

`firmware/BaudHound_Companion/` is a **device-under-test simulator** for a spare ESP32-WROOM. It emits console-style traffic and answers commands so you can exercise BaudHound end-to-end (autodetect, wake-probe, RX/TX swap, live baud changes).

Wire it to BaudHound:
```
WROOM GPIO17 (TX2) -> BaudHound GPIO16 (RX)
WROOM GPIO16 (RX2) <- BaudHound GPIO17 (TX)
GND               <-> GND
```
Its BOOT button cycles the link baud (to test re-detection); its USB port is a local debug console. See the sketch header for the full command list.

---

## Troubleshooting

**Board sits at `waiting for download` / won't boot until RESET**
This is the ESP32-S3 native-USB auto-reset quirk — the PC's serial DTR/RTS lines hold the chip in download mode. Boot mode is decided *before* firmware runs, so it's not a firmware bug.
- **Press RESET/EN once** after flashing.
- **Deployment:** power from a **USB charger / power bank** (no data lines) → boots clean every time.

**Nothing shows in the console, but the target is transmitting**
- Run `~baud` (data must be flowing). If it can't lock, try `~probe on`.
- Reversed cable? Run `~swap`.
- Check the **onboard LED**: red flashes mean bytes *are* arriving.

**Garbage flood**
- Wrong baud → pin the correct one. Same-speed but unreadable → the port is **binary** (no text to show).

**Joining the AP opens `msn.com` (Windows/Edge)**
Modern browsers use DNS-over-HTTPS, which bypasses the device's captive DNS — so the browser may open its homepage instead of the console. Reliable options: click the **"Sign in to this network"** notification, or just browse to **`192.168.4.1`**.

**Compile error: `mbedtls_*_ret` / `Serial2 not declared`**
- `mbedtls_*_ret` → you have the old `me-no-dev` async libs; install the **ESP32Async** forks instead.
- `Serial2` → the S3 core exposes `Serial0`/`Serial1`; this firmware already uses `Serial1`.

---

## Security note

The AP ships **open** (no password) for convenience. Anyone in range can reach the console — and therefore read/write whatever target UART is attached. Fine for a bench; add a password (`AP_PASS` + `WiFi.softAP(apName, AP_PASS)`) before leaving it connected to anything sensitive.

---

## License

MIT — see [LICENSE](LICENSE).
