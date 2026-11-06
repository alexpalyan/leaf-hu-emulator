# Known units

What is known about each head unit model run in the emulator so far. The
launcher picks the machine from the `QY7` or `QY8` prefix of the model in the
`PROD` block, so units not listed here may run too; `qemu-clarion --identify
NOR` prints the model and firmware build to report.

| QY model | CPU | Machine | Emulator | State |
| --- | --- | --- | --- | --- |
| QY7201NE | SH-4A | `clarion-qy7` | `qemu-system-sh4` | boots to the user interface with its map card: consent screen, map screen |
| QY7214NL | SH-4A | `clarion-qy7` | `qemu-system-sh4` | firmware analysed; boot not documented yet |
| QY7221NL | SH-4A | `clarion-qy7` | `qemu-system-sh4` | boots to the user interface with the paired map card: consent screen, touch input, map screen; MENU, MAP/VOICE, STATUS and ZERO EMISSION keys arrive over the vehicle CAN bus |
| QY8202NA | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | boots to the consent screen |
| QY8202NC | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | same board as QY8202NA (TMA616 touch); boots to the consent screen, touch accepted |
| QY8252NF | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | same board as QY8202NA (TMA616 touch); boots to the consent screen, touch accepted |
| QY8202PB | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | e-NV200, same board as QY8202NA (TMA616 touch); boots to the consent screen |
| QY8600NE | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | same board as QY8652NB (TMA460 touch); boots to the consent screen |
| QY8602NB | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | same board as QY8652NB (TMA460 touch); boots to the consent screen |
| QY8605NB | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | ZE1 board (TMA460 touch); boot not documented yet |
| QY8652NB | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` | boots to the user interface: consent screen, menu, touch input, map screen with the map card |

## QY8 board selection

The QY8 machine picks the board from the firmware build named in the NK2
image header, not from the model: `G214…` builds are for the older board
(TMA616 touch), `G218…` builds for the ZE1 board (TMA460 touch).
