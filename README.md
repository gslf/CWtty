# CWtty

**Morse telegraphy in terminal.** CWtty is a keyboard-driven TUI for CW.  Written in portable C11 for Linux, MacOS and Windows. Its dependencies are vendored in `third_party/`: [miniaudio](https://miniaud.io) for the audio, [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) and [Monocypher](https://monocypher.org) for the online rooms.

**[Try it in your browser](https://gslf.it/cwtty/)**: nothing to install.

## Install

**Linux and macOS**

```sh
curl -fsSL https://raw.githubusercontent.com/gslf/CWtty/main/install.sh | sh
```

**Windows** (PowerShell)

```powershell
irm https://raw.githubusercontent.com/gslf/CWtty/main/install.ps1 | iex
```

Then start it with `cwtty`. The script downloads the prebuilt binary of the latest [release](https://github.com/gslf/CWtty/releases) (Linux x86_64 and ARM64, macOS, Windows x64) and puts it in `~/.local/bin` or in `%LOCALAPPDATA%\Programs\cwtty` (added to the PATH). No compiler, no administrator rights. Run it again to update.

To uninstall:

```sh
curl -fsSL https://raw.githubusercontent.com/gslf/CWtty/main/install.sh | sh -s -- --uninstall
```

```powershell
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/gslf/CWtty/main/install.ps1))) -Uninstall
```


## Features

- **Transmit** – Space is a straight key, the left and right arrows are the dit and dah paddles of an electronic keyer (Iambic A, Iambic B, Ultimatic, Bug and Straight key are supported). A USB paddle or straight key adapter that acts as mouse buttons can replace the keyboard. The sidetone is click-free (raised cosine envelope) and the keyer runs inside the audio callback, so element and space lengths are sample-accurate.
- **Receive** – Decodes CW from the microphone. Sensitivity, pitch, filter width, AFC and speed lock can be changed live. A waterfall, an envelope trace, the decoded key tape and a level meter show what the decoder sees.
- **TX/RX (transceiver)** – Receives by default, touch the key and it switches to transmit, muting the receiver, then returns to receive afterthe break-in delay. 
- **Training** – Learn to copy (CWtty sends, you type) or to send (CWtty shows, you key). Copying has four levels: an item can be heard four times at level 1, down to once at level 4, and a counter shows the listens left. Your best streak of correct answers is a record (one per copy level, one for sending), kept across sessions: beat it and a big NEW RECORD appears.
- **Statistics** – Records, answers and accuracy, time spent, listens per item, sending speed and dah/dit ratio, days practised and the daily streak, the activity of the last four weeks and how well each character goes. Export them to a file, import them back, or keep every device in sync through your [Filen](https://filen.io) account, end-to-end encrypted. See [Statistics on several devices](#statistics-on-several-devices).
- **Online rooms** – Key together with other stations over the internet, from the terminal or the web app: everybody in a room hears everybody with the keying's own timing, each station on a pitch of its own, decoded on a line with its callsign. A password makes a room private and encrypted end to end. See [Online rooms](#online-rooms).
- **Morse table** – A floating reference table.
- **Settings** – Tone, speed, Farnsworth, keyboard, USB pad or USB straight key, paddle mode, volume, envelope, receiver defaults, break-in delay, audio devices and buffer size. Saved automatically.



## Terminal requirements

Telegraphy needs to know exactly when a key is pressed **and released**, something classic terminals never report. CWtty uses:

- the **kitty keyboard protocol** (with key release events) on Linux and macOS: kitty, foot, WezTerm, Ghostty, Alacritty, iTerm2 and others (WezTerm needs `config.enable_kitty_keyboard = true`);
- the native **Windows console API** on Windows (Windows Terminal or the classic console, Windows 10 or later).

The header shows `KEYS kitty` or `KEYS win32` when everything is precise. In other terminals (`KEYS legacy`), releases are guessed from the keyboard auto-repeat: the paddles still work (a tap is one element, holding repeats), but the straight key is approximate. Inside tmux or screen the protocol is usually not passed through.



## Usage

```
cwtty [options]

  -m, --mode MODE     start in tx, rx, trx, rooms, train or stats
  -c, --config FILE   use FILE for the settings
      --demo          feed the receiver with a simulated band
      --legacy-keys   do not use the kitty keyboard protocol
      --list-devices  list the audio devices and exit
  -v, --version       print the version and exit
  -h, --help          show this help

Rooms without the interface:
      --net-listen    print what is keyed in a room (Ctrl-C to stop)
      --net-send TEXT key TEXT into a room, then exit
      --room NAME     the room (default: the last one entered)
      --call CALL     your callsign (default: the one in the settings)
      --password P    enter the private room NAME with password P
```

Settings are stored in `$XDG_CONFIG_HOME/cwtty/cwtty.conf` (Linux, default `~/.config/cwtty/cwtty.conf`), `~/Library/Application Support/cwtty/` (macOS) or `%APPDATA%\cwtty\` (Windows). The training records and statistics are saved after every answer in `cwtty.stats`, next to the settings, and the Filen session (if you sign in) in `cwtty.filen`.

## Web app

CWtty also runs in the browser at **[gslf.it/cwtty](https://gslf.it/cwtty/)**, as an installable PWA (works offline). On a touch screen the Dit, Key and Dah buttons are the key.

It is the same C core compiled to WebAssembly. The microphone and the offline mode need HTTPS (or localhost). Settings, records and statistics are kept in the browser, and can be exported, imported or synced with the terminal through Filen. The online rooms work in the web app too, and its users and the terminal's meet in the same rooms.


## Building from source

You need CMake 3.16+ and a C11 compiler (Visual Studio or MinGW on Windows).

```sh
make            # build build/cwtty (Release)
make test       # run the test suite
make install    # install to /usr/local (PREFIX=... to change)
```

Without make: `cmake -S . -B build` and `cmake --build build --config Release`.

The Filen client is tested offline against a fake server (`test_filen`), and its cryptography byte for byte against values printed by Filen's official SDK (`tests/filen_vectors.h`, made by `tools/filen-vectors.mjs`; the SDK is not needed to build or test, nor part of CWtty). After `make -C web`, `node tests/test_wasm_filen.mjs build-web/cwtty.wasm` checks the same client as built into the web app.

### Releasing

Set the new `VERSION` in `CMakeLists.txt`, commit, then run `make release`: it runs the tests, tags `vX.Y.Z` and pushes the tag. The [release workflow](.github/workflows/release.yml) builds the binaries for every platform and publishes them as the GitHub release that the install scripts download.


## Online rooms

`C-x n` (or `-m rooms`) opens the Rooms screen: enter your callsign and the name of a room, optionally a password, and key. Everybody in the same room hears you, whether they use the terminal or the web app.

- **What travels** is the timing of the key, not text: every key-down and key-up with its time, in packets of up to 120 ms. The others play it on a voice of their own (a pitch per station, spread around *Remote pitch* in the settings) after a small jitter buffer, so your fist is heard as it is, and decode it on a line with your callsign. The delay is a few hundred milliseconds.
- **No server of ours.** The rooms are topics on two MQTT brokers at once: an [EMQX Serverless](https://www.emqx.com/en/cloud/serverless-mqtt) deployment (TLS) and HiveMQ's public broker. Every packet goes to both and duplicates are dropped, so a room keeps working while one of them is unreachable. The brokers are set in `src/brokers.c`.
- **Ports.** The terminal app connects to 8883 (TLS) and 1883; the web app uses secure WebSockets on 8084 and 8884. Some networks block them: the Rooms screen shows each broker's state.
- **Privacy.** A public room is like a frequency: anyone who knows its name can listen. A password makes the room private: the key is derived from it with Argon2, the topic changes and every packet is encrypted and authenticated (XChaCha20-Poly1305), so the brokers and anyone without the password see nothing. Since the topic can be watched, a weak password can be guessed offline: choose an uncommon one.
- **Turns.** Each station writes on a line of its own, as a transmission on the air: **K** passes the turn and ends the line (so do KN, BK, AR and SK), BT (`=`) starts a new paragraph. A line also ends when another station starts transmitting, even without K, or after a pause of five word spaces at the station's speed (at least 4 s). Two stations keying at once keep a line each.
- Leaving the Rooms screen leaves the room.

`--net-listen` and `--net-send` use a room without the interface, which is handy for scripts and for testing:

```sh
cwtty --net-listen --room CQ --call SWL
cwtty --net-send "CQ CQ DE IZ0ABC K" --room CQ --call IZ0ABC
```

## Statistics on several devices

Every device keeps its own section of the statistics, and what you see is their sum: train on the terminal at home and in the web app on the phone, and the totals count both. Days practised are counted once even when two devices trained on the same day, and a record is a record of all of them (NEW RECORD means you beat your best anywhere).

- **Export / import.** On the Statistics screen, `e` writes `~/cwtty-stats-YYYY-MM-DD.txt` (the web app downloads it) and `i` reads one back. Importing **replaces** the statistics here, after asking: this device takes its own section from the file (or starts empty), the other devices' sections come from the file. With the cloud on, the latest sections of the devices still in use come back at the next sync.
- **Filen cloud.** `c` on the Statistics screen (the *Cloud* button in the web app) signs in to your Filen account, two-factor codes included. Each device then keeps its section in `CWtty/stats-v1/<device>.stats` of your Filen drive: it syncs at start, a minute after you stop training, on *Sync now* and when you quit, downloading only what changed. Filen keeps earlier contents as file versions.
- **Privacy.** Like Filen's own apps, CWtty derives the keys from your password on your device (PBKDF2 or Argon2id) and encrypts every file and name before upload: Filen stores only ciphertext. The password is never stored. The terminal app keeps the session (an API key and the derived keys) in `cwtty.filen`, next to the settings, readable by you alone; the web app keeps it in the browser, encrypted with a key that cannot be read back (uncheck *Stay signed in* to keep it only until the page closes). *Disconnect* forgets it.
- **The client.** CWtty speaks Filen's protocol with its own small C client (`src/filen.c`, `src/statsync.c`, `src/fcrypto.c`), the same code in the terminal and in the web app; it is not affiliated with Filen. Accounts created before 2021 (authentication version 1) are not supported: changing the password on filen.io upgrades them. Filen's terms ask for fair use of the service: CWtty only moves a few small files.

The text format is the one of `cwtty.stats`: a `[device <id> rev=<n> name=<name>]` header before each device's `key=value` lines, the revision growing with every change so that the newest copy of a section wins.

## The receiver

The receive chain works on 8 kHz mono audio:

1. a quadrature mixer brings the tuned pitch to zero frequency;
2. a 4th order Butterworth low-pass on I and Q sets the filter width;
3. the magnitude is taken at 1 kHz and lightly smoothed (a fraction of a dit, following the measured speed);
4. the noise floor and its spread are measured with quantile trackers (robust to the signal itself, so the receiver cannot lock itself out);
5. the threshold sits halfway between noise and the mean mark level in linear amplitude, never closer to the noise than the sensitivity margin;
6. a **squelch** opens only when the marks stand far enough above the noise (the SNR shown on screen); together with the margin this is what the sensitivity control moves;
7. short glitches are removed and the key-down / key-up transitions go to the decoder.

The decoder estimates dit and dah lengths with a two-cluster k-means over the recent marks, compensates the weighting introduced by filters, splits characters at 2 units and words at 5 units, and keeps tracking the speed of the station. A 512-point FFT feeds the waterfall and the AFC.

**Sensitivity** (1–10, default 6): at 6 and below a pure noise band produces at most a stray character now and then. Above 6 the receiver catches weaker signals at the price of frequent false characters. 


### SDR integration

The transceiver's `ptt_on()` / `ptt_off()` in `src/mode_trx.c` are where a radio backend would key its transmitter. Key transitions already flow as timestamped events from the audio engine (`audio_poll_events()`), and the receiver accepts any 8 kHz audio stream through `dsp_process()`, so an SDR can provide both the receive audio and the transmit keying.

## License

CWtty is released under the [MIT License](LICENSE).
miniaudio is © David Reid, available under the public domain or MIT No
Attribution licence (see `third_party/miniaudio/LICENSE`).
Mbed TLS is © The Mbed TLS Contributors, under the Apache License 2.0 (see
`third_party/mbedtls/LICENSE`).
Monocypher is © Loup Vaillant and contributors, under CC0-1.0 or the
2-clause BSD licence (see `third_party/monocypher/LICENCE.md`).
