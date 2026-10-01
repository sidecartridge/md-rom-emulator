# Breakpoints on what the code audit suspects. Each prints and continues, so
# `swd.py gdb --script tools/dev/gdb/suspects.gdb --seconds N` logs them while
# the RP keeps working. Each stops at the function's first instruction, where
# the arguments are still in r0-r3. Code in RAM (flash_range_erase,
# flash_range_program) takes software breakpoints; code in flash takes one of
# the RP2040's four hardware breakpoints, which GDB can only set on a live
# target. panic and a hard fault stay stopped, for a backtrace.

# Flash offsets (from XIP_BASE) of the regions in rp/src/memmap_rp.ld.
set $rom_temp = 0x100000
set $booster = 0x120000
set $config = 0x1E0000

define flash-region
  if $arg0 >= $rom_temp && $arg0 + $arg1 <= $booster
    printf "ROM_TEMP"
  else
    if $arg0 >= $config && $arg0 + $arg1 <= 0x200000
      printf "config"
    else
      printf "OUTSIDE ROM_TEMP AND CONFIG: Booster or the app"
    end
  end
end

break *flash_range_erase
commands
  silent
  printf "flash_range_erase 0x%06x +%u, sp 0x%08x: ", $r0, $r1, $sp
  flash-region $r0 $r1
  printf "\n"
  continue
end

break *flash_range_program
commands
  silent
  printf "flash_range_program 0x%06x +%u: ", $r0, $r2
  flash-region $r0 $r2
  printf "\n"
  continue
end

hbreak *storeFileToFlash
commands
  silent
  printf "storeFileToFlash(\"%s\", 0x%08x), sp 0x%08x\n", (char *)$r0, $r1, $sp
  continue
end

hbreak *panic
commands
  silent
  printf "panic: %s\n", (char *)$r0
  bt 8
end

# A hard fault: fatfs-sdk's crash handler (crash.c), with the stacked frame.
hbreak Hardfault_HandlerC
commands
  silent
  printf "HardFault, frame at 0x%08x: pc 0x%08x, lr 0x%08x\n", $r0, *(unsigned int *)($r0 + 24), *(unsigned int *)($r0 + 20)
  bt 8
end
