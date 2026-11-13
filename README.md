# HDHomeRun TUI

**Version 0.9.1** · See [CHANGELOG.md](CHANGELOG.md) for what's new.

A terminal (ncurses) tool for watching and working with HDHomeRun tuners on Linux, written to replace the HDHomeRun GUI, which is no longer supported on Linux. It has the GUI's features plus a few extras, most notably one-key capture of stream samples for ATSC 1.0 and ATSC 3.0.

Version 0.9.1 also supports USB ATSC 3.0 tuners that use the cxd28xx Linux driver, alongside HDHomeRun devices.

Written with Google Gemini and Anthropic Claude, prompted by Mark J. Colombo of [RabbitEars.info](https://www.rabbitears.info/).

## Features

- Lists every tuner on your network, plus any local USB tuners, in one view
- Live signal strength, signal quality and symbol quality bars, with dBm and dB readings where the tuner reports them
- TSID (ATSC 1.0) or BSID (ATSC 3.0), the network rate, and the program list. Encrypted programs are shown in red
- ATSC 3.0 PLP status, color-coded by lock state, plus a detailed PLP and L1 screen with SNR requirements that you can save to a text file
- Channel stepping and seeking, manual tuning by channel or frequency, ATSC 3.0 PLP selection, and switching channel maps
- Live viewing in VLC
- 30-second captures: transport streams (ATSC 1.0), and HDHomeRun debug or ALP-PCAP files (ATSC 3.0). The "error checking" capture modes restart automatically until they get a clean 30 seconds. ATSC 3.0 captures also save a matching `.txt` file of PLP and L1 details, ready to feed into [RENDER](https://github.com/YOUR-USERNAME/render)

## Building

You need a C compiler, `make`, the ncurses development library, and the SiliconDust `libhdhomerun` source in a subfolder named `libhdhomerun`.

On Debian/Ubuntu:
```bash
sudo apt install build-essential libncurses-dev
```

On Red Hat/Fedora:
```bash
sudo dnf install gcc make ncurses-devel
```

On macOS (Homebrew):
```bash
brew install ncurses
```

Then:
```bash
git clone https://github.com/YOUR-USERNAME/hdhomerun_tui.git
cd hdhomerun_tui
git clone https://github.com/Silicondust/libhdhomerun.git
make
```

`make install` copies the program to `/usr/local/bin` (run it with `sudo`).

### USB tuners (optional)

USB tuners based on the Sony CXD28xx chips, such as the GT Media/Zenview HDTV Mate, need the out-of-tree cxd28xx driver with raw ALP support from [YOUR-USERNAME/cxd28xx](https://github.com/TripEricson/cxd28xx), a fork of [majortom9/cxd28xx](https://github.com/majortom9/cxd28xx) that adds the `/dev/alpraw` device. See that repo's README for setup, including the udev rule. The TUI uses `/dev/dvb` and `/dev/alpraw`. If no such tuner is present, the TUI simply doesn't list one.

## Usage

```bash
./hdhomerun_tui            # discover tuners automatically
./hdhomerun_tui -i <ip>    # connect straight to a device by IP, skipping discovery
./hdhomerun_tui -h         # show command-line help
```

Captures and detail files are saved in the current directory, with names built from the RF channel, the TSID or BSID, the PLPs and the time.

### Keys

| Key | Action |
|---|---|
| Up / Down | Select tuner |
| Left / Right | Change channel |
| PgUp / PgDn | Scroll the status panel |
| + / - | Seek to the next / previous active channel |
| 0–9 or c | Tune to a channel or frequency |
| m | Change the tuner's channel map |
| p | Set the tuned ATSC 3.0 PLPs |
| v | View the stream in VLC (pick a program); press again to stop |
| d | ATSC 3.0: detailed PLP and L1 information with SNR requirements (s inside it saves to a file) |
| s | Save a 30-second TS (ATSC 1.0) or debug (ATSC 3.0) capture |
| a | Same as s, restarting automatically on errors |
| x | ATSC 3.0: save a 30-second ALP-PCAP capture, if supported |
| z | Same as x, restarting automatically on errors |
| Backspace | Abort a save in progress |
| r | Rediscover devices |
| i | Add a device manually by IP address |
| w | Toggle mouse capture (turn off to select text in the terminal) |
| h | Help |
| q | Quit |

## Credits

- ATSC 3.0 L1 parsing adapted from `l1dump.c` in drmpeg's [dtv-utils](https://github.com/drmpeg/dtv-utils), licensed under GPLv3
- [libhdhomerun](https://github.com/Silicondust/libhdhomerun) © Silicondust USA Inc., licensed under the LGPL

## License

Copyright © 2026 Mark J. Colombo

This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE.txt](LICENSE.txt).
