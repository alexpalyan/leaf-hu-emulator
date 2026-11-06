# Known units

Head unit models known to run in the emulator. The
launcher picks the machine from the `QY7` or `QY8` prefix of the model in the
`PROD` block, so units not listed here may run too; `qemu-clarion --identify
NOR` prints the model and firmware build to report.

| QY model | Unit | Region | Firmware |
| --- | --- | --- | --- |
| QY7201NE | ZE0 | EUR | `X110ECNI.158` |
| QY7214NL | AZE0 | JPN | `X112JANI.141` |
| QY7221NL | AZE0 | EUR | `X112ECNI.100` |
| QY8202NA | AZE0 | EUR | `G214ELNI.062` |
| QY8202NC | AZE0 | EUR | `G214ELNI.066` |
| QY8252NA | AZE0 | EUR | `G214ELNI.062` |
| QY8252NF | AZE0 | EUR | `G214ELNI.112` |
| QY8202PB | e-NV200 | EUR | `G214ELNI.017` |
| QY8600NE | ZE1 | USA | `G218NNNI.161` |
| QY8602NA | ZE1 | EUR | `G218ENNI.094` |
| QY8602NB | ZE1 | EUR | `G218ENNI.120` |
| QY8605NB | ZE1 | JPN | `G218JNNI.271` |
| QY8652NB | ZE1 | EUR | `G218ENNI.120` |

Region follows the market letter of the firmware build: `E` Europe, `N`
North America, `J` Japan.

## Machines

| Series | CPU | Machine | Emulator |
| --- | --- | --- | --- |
| QY7 | SH-4A | `clarion-qy7` | `qemu-system-sh4` |
| QY8 | ARM (Renesas R-Car) | `clarion-qy8` | `qemu-system-arm` |

The QY8 machine picks the board from the firmware build named in the NK2
image header, not from the model: `G214…` builds are for the older board
(TMA616 touch), `G218…` builds for the ZE1 board (TMA460 touch).
