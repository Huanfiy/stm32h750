# J-Link Open Flashloader target-side CRC / verify attempt

Date: 2026-05-29

## Context

`./run.sh app-flash` programs the RT-Thread image to external W25Q64 at
`0x90000000` through a custom J-Link Open Flashloader. After the stable QSPI
loader optimization, a full app flash measured roughly:

```text
J-Link: Flash download: Total: 9.602s
Prepare: 0.123s, Compare: 4.290s, Erase: 0.850s, Program: 2.167s,
Verify: 2.131s, Restore: 0.038s
```

The remaining cost was mostly J-Link compare/verify readback. The attempted
optimization was to make the target compute CRC or perform verify inside the
flashloader so J-Link would not need to transfer the full flash contents back to
the host.

## SEGGER evidence

SEGGER documents an optional standard flash-loader entry point:

```c
U32 SEGGER_FL_CalcCRC(U32 CRC, U32 Addr, U32 NumBytes, U32 Polynom);
```

Reference: https://kb.segger.com/SEGGER_Flash_Loader#SEGGER_FL_CalcCRC

The local J-Link DLL also contains the relevant symbols and UI strings:

```text
SEGGER_OPEN_CalcCRC
SEGGER_FL_CalcCRC
SEGGER_FL_Verify
SEGGER_FL_Read
Use CRC or checksum calculation as verify method
CRC check is not supported by this flash bank. Switched to read back method.
Using CRC
```

## Attempt 1: CRC hook only

Implemented and exported both names:

```c
U32 SEGGER_OPEN_CalcCRC(U32 crc, U32 adr, U32 sz, U32 polynomial);
U32 SEGGER_FL_CalcCRC(U32 crc, U32 adr, U32 sz, U32 polynomial);
```

The function calculated reflected CRC32 over the memory-mapped QSPI window,
defaulting to polynomial `0xEDB88320` when the passed polynomial was zero.

Build output confirmed the symbols existed in the FLM:

```text
SEGGER_FL_CalcCRC
SEGGER_OPEN_CalcCRC
```

J-Link detail log confirmed symbol discovery:

```text
OFL: SEGGER_FL_CalcCRC() present @ offset ...
```

Forced CRC mode with:

```text
exec SetCompareMode=3
exec SetVerifyDownload=3
```

Result: J-Link attempted CRC mode but rejected it before using the hook:

```text
Comparing range 0x90000000 - 0x9001FFFF (2 Sectors, 128 KB), using single-block CRC calculation
CRC check is not supported by this flash bank. Switched to read back method.
```

The normal flash time stayed around the stable readback baseline:

```text
Total: 9.611s
Compare: 4.278s, Erase: 0.875s, Program: 2.162s, Verify: 2.129s
```

Conclusion: exporting `SEGGER_FL_CalcCRC` alone is insufficient for J-Link
Commander `loadbin` to treat this external QSPI Open Flashloader bank as CRC
capable.

## Attempt 2: add read + native verify hooks

Added:

```c
int SEGGER_FL_Read(U32 adr, U32 sz, U8 *buf);
U32 SEGGER_FL_Verify(U32 adr, U32 sz, U8 *buf);
```

J-Link detail log then detected the additional hooks:

```text
OFL: SEGGER_FL_Read() present @ offset ...
OFL: SEGGER_FL_CalcCRC() present @ offset ...
OFL: SEGGER_FL_Verify() present @ offset ...
Flash algorithm supports a native verify function which is used to compare the flash content.
Comparing range 0x90000000 - 0x9001FFFF (2 Sectors, 128 KB), using verify function of the RAMCode
```

Performance became much better:

```text
J-Link: Flash download: Total: 3.352s
Prepare: 0.127s, Compare: 0.113s, Erase: 0.840s, Program: 2.181s,
Verify: 0.048s, Restore: 0.040s
```

However, this path was not safe. A first version returned `adr` both for
success and for a mismatch at the first byte, causing erased flash to be
misclassified as matching:

```text
J-Link: Flash download: Bank 1 @ 0x90000000: Skipped. Contents already match
```

After correcting that ambiguity, J-Link performed program but reported verify
failure even though the fast timing remained:

```text
****** Error: Failed to verify @ address 0x90000000
J-Link: Flash download: Total: 3.346s
```

Removing the legacy `Verify` symbol but keeping `SEGGER_FL_Verify` still caused
J-Link to alter its verify path and fail later:

```text
****** Error: Verification failed @ address 0x90020000
J-Link: Flash download: Total: 4.071s
```

Conclusion: native verify can make the path fast, but the exact return contract
and call semantics used by J-Link Commander for this Open Flashloader were not
matched by the attempted implementation. It must not be kept in the production
flashloader without a precise SEGGER-compatible implementation.

## Final state after the attempt

The unsafe hooks were removed and the installed FLM was restored to the stable
optimized readback path:

```text
Init
UnInit
EraseSector
EraseChip
ProgramPage
SEGGER_OPEN_Read
BlankCheck
FlashDevice
```

`SEGGER_FL_Read`, `SEGGER_FL_Verify`, `Verify`, `SEGGER_FL_CalcCRC`, and
`SEGGER_OPEN_CalcCRC` are intentionally not exported in the final state.

## Recommendation

Do not re-enable target-side CRC/native verify in this loader unless the exact
Open Flashloader API contract is available, preferably including the modern
`SEGGER_OFL_Api` / `SEGGER_OPEN_CMD_INFO` structure definitions or a matching
SEGGER sample.

The viable next investigation would be to obtain a SEGGER Open Flashloader
sample that exports `SEGGER_OFL_Api` with `pfSEGGERCalcCRC`, then port the
QSPI read/compare implementation into that API shape. Until then, the stable
optimized readback path is the safe default.
