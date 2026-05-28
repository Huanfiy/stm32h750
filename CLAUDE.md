# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

RT-Thread 5.2.0 BSP for STM32H750VBT6 (Cortex-M7 @ 480 MHz), the MCU for the Smart B-pillar aging-test fixture. Two **independently built** firmware images cooperate at runtime:

- **Bootloader** (`bootloader/`, own `Makefile` + `linker_scripts/boot.lds`). Bare HAL, ~17 KB. Lives in **internal flash @ 0x08000000** (128 KB region). On reset it brings up QSPI W25Q64 in 1-4-4 memory-mapped mode and jumps to `APP_BASE_ADDR = 0x90000000`.
- **Main app** (root `SConstruct`). Full RT-Thread kernel + HAL drivers + finsh shell. Lives in **external W25Q64 @ 0x90000000** (8 MB QSPI XIP); `.data`/`.bss`/.stack stay in DTCM (`0x20000000`/128 KB). `Reset_Handler` copies `.data` from QSPI to DTCM and zeros `.bss` like a normal Cortex-M boot — the only QSPI-specific tweak is `SCB->VTOR = 0x90000000` and an MPU region (done in `board.c::rt_hw_board_init`).

Both images are flashed via J-Link. Internal-flash and QSPI-flash slots are **separate** — `boot-flash` and `app-flash` never overwrite each other.

## External dependencies

- `RTT_ROOT` (env) — RT-Thread kernel checkout. The user's shell exports `~/SDK/rt-thread`. `SConstruct` falls back to `../../..` which is wrong on this machine; do not rely on the fallback.
- `RTT_EXEC_PATH` (env, optional) — toolchain bin dir. Defaults in `run.sh` to `~/toolchain/arm-gnu-toolchain-13.3.rel1-x86_64-arm-none-eabi/bin`. Must contain `arm-none-eabi-*`.
- `BUILD_MODE` (env) — `Debug` (default, `-O0 -g`) or `Release` (`-O2 -DNDEBUG`). Read by `rtconfig.py`.
- `JLinkExe` on `PATH`, device `STM32H750VB`, SWD @ 4 MHz. **No OpenOCD path** — OpenOCD's `jlink` driver hangs on H7 DAP setup.
- **Custom J-Link device profile** at `~/.config/SEGGER/JLinkDevices/Custom/STM32H750VB_W25Q64/`. Installed by `make -C tools/flashloader install`. Overrides the built-in `STM32H750VB` profile's QSPI bank with a W25Q64-aware Open Flashloader; internal-flash bank stays on the built-in algorithm. **The user-level XML must use `Name="STM32H750VB"` (same as the built-in)** — using a new name causes J-Link to skip the H7-specific connect sequence and fail at "Failed to power up DAP".

## Commands

Everything goes through `run.sh` (do not call `scons` directly unless debugging the build system):

```
# Main app (QSPI XIP)
./run.sh build              # scons -j$(nproc), pipes through `bear` → .vscode/compile_commands.json
./run.sh clean              # scons -c + rm -rf build/
./run.sh app-flash          # JLink loadbin build/rt-thread.bin → 0x90000000 via custom .FLM
./run.sh app-rebuild-flash
./run.sh flash              # legacy: writes build/rt-thread.bin to 0x08000000 (would overwrite bootloader!)
./run.sh reset              # JLink reset+go, no flashing
./run.sh rebuild[-flash]

# Bootloader (internal flash)
./run.sh boot-build         # make -C bootloader -j$(nproc), output bootloader/build/bootloader.{elf,bin}
./run.sh boot-clean
./run.sh boot-flash         # flashes bootloader.bin → 0x08000000
./run.sh boot-rebuild[-flash]

./run.sh <cmd> -v           # show full scons output (default is --silent)
```

Two-step end-to-end first-time setup:

```
./run.sh boot-rebuild-flash       # populate internal flash
./run.sh app-rebuild-flash        # populate external QSPI
./run.sh reset                    # bootloader will pick up the app and jump
```

After a successful build, `post_build.py` prints a Flash/RAM usage bar derived from `arm-none-eabi-size` plus the `MEMORY {}` block in the linker script. The parser handles both `K` and `k` length suffixes — when changing `link.lds`'s `ROM`/`RAM` sizes, the percentages update automatically.

`menuconfig` is the standard RT-Thread Env workflow on this BSP (`Kconfig` → `.config` → `rtconfig.h`). No wrapper command exists in `run.sh`.

## Architecture

### Build wiring

`SConstruct` is the standard RT-Thread BSP shape:
1. `PrepareBuilding(env, RTT_ROOT, has_libcpu=False)` pulls in kernel + components per `rtconfig.h`.
2. `libraries/STM32H7xx_HAL/SConscript` adds the ST HAL.
3. `libraries/HAL_Drivers/SConscript` adds the RT-Thread STM32 driver shims (`drv_gpio.c`, `drv_usart.c`, …) gated by `BSP_USING_*` Kconfig options.
4. `board/SConscript` adds `board.c` + the CubeMX-generated `stm32h7xx_hal_msp.c` and `system_stm32h7xx.c` + the GCC startup `.s`. **All paths are relative**, otherwise SCons's `variant_dir` doesn't apply and `.o` files end up next to the source instead of under `build/`.
5. `applications/SConscript` adds user code (`main.c`).

The CubeMX project lives at `board/CubeMX_Config/`. Commit `1b1ce84` switched `system_stm32h7xx.c` from the BSP-provided template to the CubeMX-generated one — when regenerating from `.ioc`, keep that file under `board/CubeMX_Config/Core/Src/` (the SConscript globs it from there explicitly, **not** from the HAL templates dir).

`SCons` is invoked by `bear`; the generated `compile_commands.json` is moved into `.vscode/` for clangd. The repo's IntelliSense is clangd-only — `C_Cpp.intelliSenseEngine` is `disabled` in `.vscode/settings.json`.

### Memory layout (main app)

`board/linker_scripts/link.lds`:
- `ROM` = 8 MB @ `0x90000000` (W25Q64 via QUADSPI memory-mapped)
- `RAM` = 128 KB @ `0x20000000` (DTCM)
- `_system_stack_size = 0x800` — startup stack only; RT-Thread switches to per-thread stacks after scheduling begins. **Do not lower below 0x800** — `SystemClock_Config → PeriphCommonClock_Config → HAL_RCCEx_PeriphCLKConfig → RCCEx_PLL2_Config → HAL_GetTick` peaks at ~700 B; the prior `0x200` setting silently overflowed the stack into `.data`'s tail, corrupting `_object_container[Memory]` and tripping `rt_smem_init`'s `RT_ASSERT(information != NULL)`. See `.agent/fixed/2026-05-28-app-startup-stack-overflow-corrupts-object-container.md`.

`board/board.h` mirrors these as `ROM_*`/`RAM_*` macros. `board.c::init_sram` additionally registers AXI/SRAM1/SRAM2/SRAM3/SRAM4/BACKUP regions as `rt_memheap`s under `INIT_BOARD_EXPORT(init_sram)` when `RT_USING_MEMHEAP` is set — so multiple named heaps exist at runtime (`axi_sram`, `sram1`, …) on top of the small DTCM system heap.

`board.c` defines a **strong** `rt_hw_board_init` that overrides the `rt_weak` one in `libraries/HAL_Drivers/drv_common.c`. The override prepends two QSPI-XIP-specific steps before the upstream body:
1. `mpu_config_qspi_xip()` — MPU region 0 covers 8 MB at `0x90000000`, Normal + Cacheable + WBnRA. Without this the CPU treats QSPI as Device memory, hurting fetch throughput and breaking unaligned access.
2. `SCB->VTOR = ROM_START` — must run before `HAL_Init()` because `HAL_Init` arms the SysTick interrupt and any subsequent IRQ dispatches through whatever VTOR was in effect at that moment.

The dead `ota_app_vtor_reconfig` helper has been removed.

### Bootloader

`bootloader/src/main.c` flow: MPU config (mark `0x90000000`+8 MB as Normal/Cacheable for XIP) → `HAL_Init` → 480 MHz PLL → UART1 log → QSPI init → `w25q64_reset` (covers stuck-in-QPI warm boots, but not from QPI itself — that requires a power cycle) → JEDEC check (`0xEF4017`, hard-fail gated by `BOOT_REQUIRE_JEDEC_OK` in `inc/boot_config.h`) → set QE → memory-map (1-4-4, `0xEB` Fast Read Quad I/O with mode-byte = `0xF0`) → sanity-check app SP/PC (SP in DTCM range, PC in QSPI range with thumb bit set) → `SCB->VTOR` + `__set_MSP` + jump.

The bootloader's `Makefile` is **standalone** — it does not share build state with the root `SConstruct`. It picks a minimal HAL module list (UART/QSPI/RCC/PWR/GPIO/DMA/MDMA/EXTI/CORTEX) and links against `bootloader/linker_scripts/boot.lds` (128 KB ROM / 128 KB DTCM, 2 KB main stack, full `.init`/`.fini`/`.preinit_array`/`.init_array`/`.fini_array` so `__libc_init_array` returns cleanly — see `.agent/fixed/2026-05-28-bootloader-hardfault-no-uart.md`). The Makefile mirrors the layout of `tools/flashloader/Makefile` so all "small bare-metal" build products in the repo share one engine.

The RT-Thread-patched `startup_stm32h750xx.s` jumps to `entry` (RT-Thread internal symbol), not `main`. `bootloader/src/stubs.c` supplies a trampoline `int entry(void) { return main(); }` so the same startup file can drive a non-RT-Thread image.

### Flash loader (J-Link Open Flashloader)

`tools/flashloader/` is a third standalone build product, separate from both bootloader and main app:

- `FlashPrg.c` — `Init/UnInit/EraseSector/EraseChip/ProgramPage/BlankCheck/SEGGER_OPEN_Read`, raw QUADSPI register pokes only (no HAL, no libc). Runs in AXI-SRAM `0x24000000` when J-Link loads it.
- `FlashDev.c` — `FlashDevice` struct describing W25Q64JV (base `0x90000000`, 8 MB, 4 KB sectors, 256 B page).
- `flashloader.lds` — links into `RAM(rwx) 0x24000000 / 32 KB`. Uses `EXTERN(Init UnInit EraseSector EraseChip ProgramPage BlankCheck SEGGER_OPEN_Read FlashDevice)` plus per-function `__attribute__((used))` so `--gc-sections` does not drop J-Link's entry points.
- `JLinkDevices.xml` — `ChipInfo Name="STM32H750VB"` (same as built-in!), `FlashBankInfo Loader="STM32H750VB_W25Q64.FLM" LoaderType="FLASH_ALGO_TYPE_OPEN"`.
- `Makefile` — `make` builds the .FLM, `make install` copies the .FLM + XML to `~/.config/SEGGER/JLinkDevices/Custom/STM32H750VB_W25Q64/`.

`SEGGER_OPEN_Read` is **mandatory** — without it J-Link's compare/verify pass falls back to direct SWD reads of `0x90000000`, which return 0xFF because `Init()` leaves QSPI in indirect (non-memory-mapped) mode. See `.agent/fixed/2026-05-28-jlink-qspi-w25q64-flashloader.md` for the full debugging trail.

## Debugging tips

When a build silently hangs after `boot-flash` / `app-flash` / `reset`:
1. `JLinkExe` → `connect`, `h`, `regs` — read `PC`, `IPSR`, fault registers (`E000ED28` CFSR, `E000ED2C` HFSR, `E000ED34` MMFAR).
2. `arm-none-eabi-addr2line -e <elf> -f -C <PC>` to map PC to function.
3. If PC sits in `rt_assert_handler`'s spin loop with `IPSR=NoException`, scan the MSP frame (`mem32 <MSP> 32`) for return addresses pointing into QSPI/internal flash, then `addr2line` each candidate to recover the call chain — the original `LR` is gone because the assert handler's own stack frame trampled it.
4. If you suspect `.data` corruption: `objcopy -O binary --only-section=.data <elf> /tmp/data.bin`, J-Link `savebin /tmp/data-ram.bin 0x20000000 0x<size>`, then `cmp -l` to find the byte offsets that differ from initial values.

`.agent/workflow/stm32h7-jlink-gdb-serial-closed-loop-debug.md` documents the full closed-loop debug protocol (J-Link + GDB + serial); `.agent/fixed/` accumulates root-cause writeups for past incidents — review before starting a new investigation in a similar area.

## Testing

The `test/` directory holds Python-based closed-loop test harnesses that drive
the **actual board** over J-Link SWD, USB-TTL serial, and the ZQWL UCANFD-100C
CAN box. All scripts use only the Python 3 standard library — no `pyserial` or
other third-party deps.

Reusable channel modules under `test/lib/`:

- `jlink.py` — `JLinkExe -CommandFile` wrapper: `reset_run()`, `halt_and_regs()`, `read32_many(addrs)`.
- `serial_term.py` — `Term.send_line()` (paced to ~25 char/s so finsh doesn't drop bytes), `Term.expect(regex, timeout)`.
- `zqwl_can.py` — ZQWL vendor binary protocol: `ZqwlCan(path, bitrate_code).send(id, data)` / `recv(timeout)` / `raw_drain()`. `BITRATE_500K_CLASSIC = 0x25`.

Per-feature cases under `test/cases/` are standalone executables using the
autotools-style exit code convention: **0 = PASS, 1 = FAIL, 77 = SKIP**. Each
case first probes its dependencies (`have_jlink()`, `serial_term.device_present()`,
`zqwl_can.device_present()`) and emits SKIP when hardware is missing — so the
suite degrades gracefully on a machine without the full rig connected.

```
python3 test/run_all.py             # run every case under test/cases/, summary table
python3 test/cases/test_adc.py      # run one case in isolation
```

`test/run_all.py::CASE_ORDER` controls execution order; `test_swd.py` /
`test_boot.py` reset the target first, later cases assume the app is already
running and only drive msh.

## Test-driven delivery

**Every new feature, driver, or behaviour change must ship with a closed-loop
case under `test/cases/` that exercises it end-to-end on real hardware** — and
that case must PASS before the implementing commit lands. Add the case
filename to `test/run_all.py::CASE_ORDER` so it runs by default.

Loop: write a failing case → implement → case passes → commit (case + impl
together, single commit). Don't ship driver code in one commit and tests in a
follow-up — the case is the acceptance criterion, not an afterthought.

Exemptions are allowed but must be explicit. If something genuinely cannot be
closed-loop-tested — physical wiring sanity, ISR-latency budgets that can't be
observed from PC side, third-party SDK plumbing that has no observable surface
— call it out in the commit body with the phrase `test-exempt: <reason>`. CI /
reviewers grep for that token. Hardware-availability-related SKIPs (no J-Link,
no CAN box) are not exemptions — they're built into the harness and count as
runs.

## Conventions

- Toolchain is GCC-only (`PLATFORM = 'gcc'`, `CROSS_TOOL = 'gcc'`). The Keil/IAR branches in `rtconfig.py` were removed.
- `RT_NAME_MAX = 8` — RT-Thread object names (threads, semaphores, devices) truncate at 8 chars.
- All board-level peripheral enables go through `board/Kconfig`'s `BSP_USING_*` flags; the underlying `RT_USING_*` Components flags are `select`ed from there. Don't toggle the `RT_USING_*` ones directly when adding a peripheral — flip the BSP flag and let Kconfig propagate.
- The flash/debug device string is `STM32H750VB` for J-Link CLI (both `boot-flash` and `app-flash`), `STM32H750VBTx` for cortex-debug (`.vscode/launch.json`). Don't unify them — the two tools expect different forms.
- `bootloader/` and `tools/flashloader/` are independent Make-based builds; **never** add them to the root `SConstruct` (and never let the root `SConstruct`'s subdir-scanning logic in `board/SConscript` reach them either — it auto-pulls any nested `SConscript`, which would multiple-define `main` / `HAL_QSPI_MspInit` / `FlashDevice`).
- The user-level J-Link `JLinkDevices.xml` lives outside the repo (`~/.config/SEGGER/JLinkDevices/Custom/`). The source of truth is `tools/flashloader/JLinkDevices.xml`; sync via `make -C tools/flashloader install`. On a fresh machine, `./run.sh app-flash` will fail until that's done.
