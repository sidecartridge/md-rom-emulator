# Changelog

## v2.1.1 (2025-12-23) - bug fix release

### Features
- No new features.

### Changes
- No functional changes.

### Fixes
- Strange bug that caused memory corruption parsing the downloaded ROM list when the microfirmware is compiled in Release mode has been fixed. Now the build script forces Debug mode to avoid this issue. Memory leak issue or overoptimization by the compiler suspected.
- Read the downloaded ROM list from the ROM folder set in the parameters, not from a hardcoded path (not the cause of the issue, but a good time to fix it).

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