# The ROM Emulator's own GDB commands, loaded by `tools/dev/swd.py gdb`.
# This GDB has no Python, so everything here is plain GDB command language.

# app-settings: the app's settings (keys and values; none of them is secret).
define app-settings
  set $ctx = &'aconfig.c'::gSettingsCtx
  set $i = 0
  while $i < $ctx->configData.count
    printf "  %-24s %s\n", $ctx->configData.entries[$i].key, $ctx->configData.entries[$i].value
    set $i = $i + 1
  end
end
document app-settings
The app's settings as loaded in RAM (FOLDER, EMULATED, MODE, the catalogs).
end

# global-settings: keys only. The global settings hold the Wi-Fi password, and
# without Python this GDB cannot tell which value to hide.
define global-settings
  set $ctx = &'gconfig.c'::gSettingsCtx
  set $i = 0
  while $i < $ctx->configData.count
    printf "  %s\n", $ctx->configData.entries[$i].key
    set $i = $i + 1
  end
end
document global-settings
The global settings' keys (Booster's). Values are left out: one is the Wi-Fi password.
end

# app-state: the setup menu's state in emul.c.
define app-state
  printf "menuLevel %d, submenuLevel %d\n", 'emul.c'::menuState.menuLevel, 'emul.c'::menuState.submenuLevel
  printf "romsCount %d, page %d of %d, downloadRomSelected %d\n", 'emul.c'::romsCount, 'emul.c'::currentRomPage, 'emul.c'::maxRomPages, 'emul.c'::downloadRomSelected
  printf "romsFolder %s\n", 'emul.c'::romsFolder
  printf "keepActive %d, delayMode %d, hasNetwork %d, wifiConnected %d, catalogAvailable %d\n", 'emul.c'::keepActive, 'emul.c'::delayMode, 'emul.c'::hasNetwork, 'emul.c'::wifiConnected, 'emul.c'::catalogAvailable
end
document app-state
The setup menu's state variables in emul.c.
end

# memory-limits: where the heap and the stack may go in this build.
define memory-limits
  printf "heap from 0x%08x (end) to 0x%08x (__StackLimit)\n", &end, &__StackLimit
  printf "cartridge window 0x%08x\n", &__rom_in_ram_start__
  printf "stack top 0x%08x, sp now 0x%08x\n", &__StackTop, $sp
end
document memory-limits
The heap's start and limit, the cartridge window and core 0's stack top.
end
