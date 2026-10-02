# Changelog

## v2.2.0 (2026-10-03) - feature release

### Features
- A new setup screen: sections in boxes with status icons, the microSD card's and Wi-Fi's state in the title bar, progress bars, and messages that clear themselves after a few seconds.
- The setup screen's options are single keys: no RETURN after them.
- Downloads over HTTPS. The catalog defaults to `https://roms.sidecartridge.com/roms.csv`; a catalog of your own can be `https://` or `http://`.
- A download screen with its progress. ESC goes back to the menu while the download goes on, and the downloaded ROM becomes the selected one.
- The catalog is kept on the microSD card and read a page at a time: any number of entries, and the list is there when the network is not.
- Wi-Fi joins again by itself when the network drops or was not there at boot.
- The microSD card can be taken out and put back while the setup screen is up.
- Falcon support.
- The Multi-device restarts by itself if it ever hangs, and the setup screen then says why.

### Changes
- **[B]rowse selects a ROM; [L]aunch loads it.** Before, choosing a ROM in the list launched it at once.
- **SHIFT no longer boots the desktop from the setup screen**, so capitals can be typed in the settings. **[E]** exits to the desktop.
- The setup screen comes up at once; the network and the catalog refresh happen behind it.
- A launch writes the ROM to flash, reads it back, and only then switches to it: a ROM that is missing, empty or larger than 128 KB is refused with the reason, before anything is changed, and the previous ROM stays.
- The app is rebuilt on the SidecarTridge Multi-device Microfirmware App Template, and compiled with full optimisation.

### Fixes
- Downloads and launches no longer crash the Multi-device: its stack overflowed into the second core's during downloads.
- A downloaded ROM whose name has spaces is saved with them (`Buggy Boy.img`, not `Buggy%20Boy.img`), and a download that does not match the catalog's size is deleted instead of selected.
- After [E] exits to the desktop, resetting the computer brings the setup screen back, instead of going to the desktop again.
- SELECT is answered during long operations (a download, a launch, the network's start), and the computer keeps its connection to the setup screen during them.
- Catalog entries with unsafe or too long file names are refused, instead of being saved somewhere else.

---

## v2.1.2 (2026-01-22) - bug fix release

### Features
- No new features.

### Changes
- Added `AGENTS.md` with guidance for LLM agents working in this repo.
- RP build now forces `MinSizeRel` to reduce memory pressure.

### Fixes
- Reduced stack usage by avoiding large stack allocations in ROM catalog parsing.

---

## v2.1.1 (2025-12-23) - bug fix release

### Features
- No new features.

### Changes
- No functional changes.

### Fixes
- Strange bug that caused memory corruption while parsing the downloaded ROM list when the microfirmware is compiled in Release mode has been fixed. Now the build script forces Debug mode to avoid this issue. Root cause is suspected memory corruption, possibly due to over-optimization by the compiler.
- Read the downloaded ROM list from the ROM folder set in the parameters, not from a hardcoded path (not the cause of the issue, but a good time to fix it).
- Improved boot after reset and booster launch by disabling the killing of core 1 until after the flash settings have been saved. This prevents random hangs during boot.

---

## v2.1.0 (2025-12-12) - release

### Features
- Added autorun mode. When a file named ".autorun" is present in the ROM folder, the filename inside that file will be automatically executed on startup. This allows launching of diagonostic cartridges or other utilities without user intervention. It is valuable for troubleshooting computers with faulty keyboards or screens.

### Fixes
- Green LED now correctly indicates when the ROM emulation is working.
- Launching booster now kills core 1 to avoid conflicts. Now it does not randomly hang the system.

---

## v2.0.2 (2025-07-02) - release
### Fixes
- Fixed issue with reset call to restart the device.

---

## v2.0.1 (2025-06-05) - release
- First version

---
