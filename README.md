# EBS211 - Mini Body Control Light

S32K144 final project, imported from the current `Final_Project` source directory.

## Project behavior

- SW1 on PTC13 cycles through OFF, 25%, 50%, 75%, and fading modes.
- FTM0 channel 0 drives PWM on PTD15, configured at 2 kHz.
- LPIT channel 0 provides a 10 ms tick. Fading updates every three ticks.
- Fixed brightness settings are saved to an 8-byte Flash record. The last 4 KiB sector at `0x0007F000` is reserved for that record.
- Flash writes are skipped when the requested duty matches the value returned by the load routine. Fading steps do not write Flash.

These describe the checked-in configuration and code intent. Hardware behavior was not retested as part of this upload.

## Build and debug

1. Open S32 Design Studio for S32K1xx and install the matching S32K1xx SDK 4.0.1 and ARM GNU toolchain.
2. Import this repository using **Existing Projects into Workspace**. The Eclipse project name is `Final_Project`.
3. Check the SDK attachment and the existing `S32K144_SDK_4.0.1_PATH` and `s32sdk_s32k1xx_rtm_401` path variables against the local SDK installation.
4. Select **Debug_FLASH** and build. The configured linker file is `Project_Settings/Linker_Files/S32K144_64_flash.ld`.
5. Use the supplied P&E debug launch configuration with a compatible S32K144 board.

The repository includes the project's existing SDK source subset, but its IDE settings still reference an external SDK installation. Build output and backup directories are omitted. A fresh build was not run for this upload.

## Known issues and validation still needed

- **Flash restore:** `main()` obtains the saved duty and sets `g_mode`, then calls `SetDuty(50U)` unconditionally. The initial PWM duty therefore becomes 50% even when another value was saved. Correct this initialization and test restoration after reset and power cycling before claiming restore works.
- **RAM build configurations:** the FLASH configurations exclude a duplicate SDK driver tree, while the RAM configurations do not have the same exclusion. Review those exclusions before using a RAM build.
- Confirm switch debounce, all brightness modes, fading, Flash save/verify, and power-cycle restoration on the actual board.

## Vendor files

NXP SDK and generated files retain their existing copyright and license notices. SDK files include NXP-specific license restrictions; this repository does not grant a new license for them or apply a blanket open-source license. Use the SDK under the applicable NXP terms.
