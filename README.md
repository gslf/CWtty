# CWtty

**Morse telegraphy in terminal.** CWtty is a keyboard-driven TUI for CW.  Written in portable C11 for Linux, MacOS and Windows. Its only dependency is [miniaudio](https://miniaud.io), vendored in `third_party/`.

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

- **Transmit** – Space is a straight key, the left and right arrows are the dit and dah paddles of an electronic keyer (Iambic A, Iambic B, Ultimatic, Bug and Straight key are supported). A USB paddle that acts as mouse buttons can replace the keyboard. The sidetone is click-free (raised cosine envelope) and the keyer runs inside the audio callback, so element and space lengths are sample-accurate.
- **Receive** – Decodes CW from the microphone. Sensitivity, pitch, filter width, AFC and speed lock can be changed live. A waterfall, an envelope trace, the decoded key tape and a level meter show what the decoder sees.
- **TX/RX (transceiver)** – Receives by default, touch the key and it switches to transmit, muting the receiver, then returns to receive afterthe break-in delay. 
- **Training** – Learn to copy (CWtty sends, you type) or to send (CWtty shows, you key).
- **Morse table** – A floating reference table.
- **Settings** – Tone, speed, Farnsworth, keyboard or USB pad, paddle mode, volume, envelope, receiver defaults, break-in delay, audio devices and buffer size. Saved automatically.



## Terminal requirements

Telegraphy needs to know exactly when a key is pressed **and released**, something classic terminals never report. CWtty uses:

- the **kitty keyboard protocol** (with key release events) on Linux and macOS: kitty, foot, WezTerm, Ghostty, Alacritty, iTerm2 and others (WezTerm needs `config.enable_kitty_keyboard = true`);
- the native **Windows console API** on Windows (Windows Terminal or the classic console, Windows 10 or later).

The header shows `KEYS kitty` or `KEYS win32` when everything is precise. In other terminals (`KEYS legacy`), releases are guessed from the keyboard auto-repeat: the paddles still work (a tap is one element, holding repeats), but the straight key is approximate. Inside tmux or screen the protocol is usually not passed through.



## Usage

```
cwtty [options]

  -m, --mode MODE     start in tx, rx, trx or train
  -c, --config FILE   use FILE for the settings
      --demo          feed the receiver with a simulated band
      --legacy-keys   do not use the kitty keyboard protocol
      --list-devices  list the audio devices and exit
  -v, --version       print the version and exit
  -h, --help          show this help
```

Settings are stored in `$XDG_CONFIG_HOME/cwtty/cwtty.conf` (Linux, default `~/.config/cwtty/cwtty.conf`), `~/Library/Application Support/cwtty/` (macOS) or `%APPDATA%\cwtty\` (Windows).

## Web app

CWtty also runs in the browser at **[gslf.it/cwtty](https://gslf.it/cwtty/)**, as an installable PWA (works offline). On a touch screen the Dit, Key and Dah buttons are the key.

It is the same C core compiled to WebAssembly. The microphone and the offline mode need HTTPS (or localhost). 


## Building from source

You need CMake 3.16+ and a C11 compiler (Visual Studio or MinGW on Windows).

```sh
make            # build build/cwtty (Release)
make test       # run the test suite
make install    # install to /usr/local (PREFIX=... to change)
```

Without make: `cmake -S . -B build` and `cmake --build build --config Release`.

### Releasing

Set the new `VERSION` in `CMakeLists.txt`, commit, then run `make release`: it runs the tests, tags `vX.Y.Z` and pushes the tag. The [release workflow](.github/workflows/release.yml) builds the binaries for every platform and publishes them as the GitHub release that the install scripts download.


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
