# TLS console

Firmware 1.4 adds an encrypted console on **TCP port 992** (telnets). It carries the same telnet stream as port 23, inside TLS 1.2, so typed commands, passwords sent to the target and the target's output cannot be read off the network.

## What it protects and what it does not

| | Plain telnet (23), web console | TLS console (992) |
|---|---|---|
| Traffic readable by others on the network | yes | no |
| Others can watch the session | yes | no, it is exclusive |
| Others can type into the session | yes | no, it is exclusive |
| Password needed to connect | no | no |
| Device identity checked | no | yes, by certificate fingerprint |

There is no password. Anyone who can reach the device can open the TLS session if it is free. Once it is open, nobody else can see or type until it ends.

## The certificate

On the first boot of 1.4 the device generates an ECDSA P-256 key and a self-signed certificate (`CN=BaudHound-XXXX, O=BaudHound`, valid 2026 to 2049) and stores both in NVS. Later boots reuse them, so the fingerprint stays the same. Key generation adds a second or two to that first boot.

The fingerprint is the SHA-256 of the certificate, base64 without padding, printed the way OpenSSH prints keys:

```
SHA256:i53p547UBDcVHZFugau3n9X6ThfqnefwLnDUkfkT+XY
```

It is shown in four places:

- **⚙ Settings**, under the WiFi status: `TLS console :992 - SHA256:...`
- `~info` on any console
- `tlsfp` in `/status`
- `tlsfp` in the discovery reply

There is no CA, so clients pin the fingerprint on first use. Compare it with the Settings page the first time you connect. Read it there over the device's own AP if you want a path nobody else is on.

## One session at a time

When a TLS session opens:

- Plain telnet clients get `[closed: a TLS session started]` and are disconnected. New ones get `[locked: a TLS session is active]` and are closed.
- The web console shows `[locked: ...]` and gets no more output.
- `/send`, `/serial`, `/wifi` and `/swap` answer `423 Locked`, so nobody can type or change settings from the web page.
- A second TLS client is closed before any handshake.
- `/status` and discovery report `"locked":true`.

USB is not paused. Its passthrough needs the device in hand.

The lock ends when the TLS client disconnects. A client that disappears without closing, for example a laptop that leaves Wi-Fi, is detected by TCP keepalive after about 25 seconds (10 s idle, then 3 probes 5 s apart). A connection that never finishes the handshake is dropped after 15 seconds.

## Connecting

**BaudHoundTerm:** press **BaudHound**, pick the device, **Connect**. It uses TLS whenever the device offers it. On first connect it shows the fingerprint and asks whether to trust it. Afterwards it connects silently, and refuses with a warning if the certificate changes. Typing `<ip>:992` in the address box also uses TLS.

**OpenSSL:**

```bash
openssl s_client -quiet -connect 192.168.4.1:992
```

`-quiet` hides the handshake output. OpenSSL does not pin; check the fingerprint yourself:

```bash
echo | openssl s_client -connect 192.168.4.1:992 2>/dev/null | openssl x509 -outform DER \
  | openssl dgst -sha256 -binary | base64 | tr -d '='
```

The console is line-based: it acts on a line when you press Enter, and does not echo. BaudHoundTerm echoes and edits locally. In `openssl s_client`, what you type shows because your terminal echoes it.

## Changing the certificate

The key lives in NVS next to the Wi-Fi and serial settings. To make a new one, erase the flash and flash the firmware again. The Wi-Fi and serial settings are erased with it:

```bash
esptool.py --chip esp32s3 erase_flash     # esptool 5.x: esptool --chip esp32s3 erase-flash
```

Every client that pinned the old certificate then warns that it changed. That warning is expected after an erase. At any other time, treat it as someone between you and the device.

## Implementation notes

- mbedTLS from the ESP32 Arduino core (3.x), on a non-blocking lwIP socket. The handshake advances one step per `loop()`, so the UART and the other consoles keep running while it completes.
- TLS 1.2 only. The core's mbedTLS is built without TLS 1.3.
- Output is framed as on port 23: lines end with CR LF and a 0xFF byte is sent as IAC IAC. Input goes through the same telnet parser, so Enter can be CR, LF, CR LF or CR NUL, and backspace edits the line.
- The TLS code adds about 77 KB of flash.
