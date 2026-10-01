VSC-55 A00 - Windows 98 SE

INSTALL
Extract the entire package. Run INSTALL.BAT from that folder on Windows 98.
Installation uses C:\VSC-55 and preserves existing ROMs and settings.
Copy your own ROMS into C:\VSC-55\ROMS, then reboot.
The tray panel starts STOPPED by default. Select firmware and press Power/Start.
Optional: check "Start emulation when VSC-55 opens" in Settings and Save.
This affects the next GUI launch; it is off by default ([Synth] AutoStart=0).
Choose VSC-55 in Windows Multimedia MIDI settings or your MIDI router/player.

ROM subfolders: MK1\1.00, MK1\1.10, MK1\1.20, MK1\1.21, MK1\2.00,
MK2\1.01, MK2\CTF1.01. ROMs are not supplied. Custom folders may be selected.

Closing the GUI hides it; playback continues. Tray Exit stops the application.
Sample rate: select in Settings while stopped; applies on the next Start.
[Audio] SampleRate accepts 11025, 16000, 22050, 32000, 44100 or 48000.
Missing/invalid values use 48000. Exit the GUI before manual INI edits,
then reopen it. GUI Save settings writes the displayed selections.
Gain: 100% unity, up to 1600%; high gain can clip. Audio default: 48 kHz,
20 ms x 8 buffers. Emulation consumes CPU even without MIDI input.
Windows shutdown should close the application automatically.

UNINSTALL
Run C:\VSC-55\UNINSTALL.BAT, then reboot. Files, settings and ROMs remain.

DOCUMENTATION
See README.md for usage, troubleshooting, architecture and source credits.
See also BUILDING.MD and CREDITS.TXT in this package.
Logs are written in C:\VSC-55; stop/exit before copying them.
The software revision is A00.

LICENSES
Code: GPL-2.0-or-later with MIT utility files. Separate faceplate artwork has
original MAME non-commercial terms. See LICENSE.TXT, PANELART.TXT, LICENSES.MD
and LICENSES directory. Matching sources: SOURCE.ZIP. ROMs are excluded.
