Put your own dumps of the three games (USA) here, under any file name
(.sfc or .smc): each is recognized by its header. `gN.sfc` wins when a
game is there twice. Any of them can be left out.

| File | Size | Mapping | SHA-1 |
|---|---|---|---|
| `g1.sfc` | 1.5 MiB | LoROM (FastROM) | `449a00631208fbcc8d58209e66d0d488674b7fb1` |
| `g2.sfc` | 1.5 MiB | LoROM + Cx4 | `637079014421563283cded6aeaa0604597b2e33c` |
| `g3.sfc` | 2 MiB | LoROM + Cx4 | `b226f7ec59283b05c1e276e2f433893f45027cac` |

No copier header (a 512-byte header is stripped if present). The ROMs are
never committed (see `.gitignore`); each one present at build time is also
recompiled to C.
