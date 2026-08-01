# Embedded wrapper cursor

`default.cur` is the original `CURSOR_DEFAULT` sword from Disciples II
`Imgs/IsoCursr.ff` (`DEFAULT.PNG`, 24x54, hotspot 0x0), converted to a native
32-bit Windows cursor. It is used only over pixels owned by the wrapper, where
the game's software cursor cannot be composited. Inside the game surface the
game continues to render its own context-sensitive cursors.

All locally tested original, MNS and SMNS installations contain the same
`IsoCursr.ff` SHA-256:
`3DFB33781790FE7888A11F84D2584B835BEC3FBAA14264021A62B8B1FDC353E1`.
