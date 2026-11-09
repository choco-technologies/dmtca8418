# Configuration

The ini section of the driver (`driver_name=dmtca8418`). The wiring is done by dmdevfs: the section is in a `friends_group` with the
I2C bus (a dmi2c section, `friend_role=i2c`) and, optionally, the light of the keys (a dmgpio section, `friend_role=backlight`) - the
group of the display of the board, so that a display service finds its inputs by the group.

| Key | Default | Meaning |
|-----|---------|---------|
| `instance` | `0` | Number of the node |
| `address` | `0x34` | 7-bit I2C address of the chip |
| `rows` | `4` | Rows of the matrix, 1..8 |
| `cols` | `10` | Columns of the matrix, 1..10 |
| `mirror_cols` | `no` | The columns are counted from the other side (the chip counts from the right edge of the T-Deck Pro keyboard) |
| `poll_ms` | `10` | Period of the polling of the chip while a client waits |
| `backlight_role` | `backlight` | `friend_role` of the line of the light |
| `debug` | `no` | Log every event of the chip (row, column, key) - to find the places of the keys of a keyboard |
| `row0` .. `row7` | none | The keys of a row, a name for each column, separated by spaces |

**The names of the keys:** a letter `A`..`Z` (capitals), a digit, `ENTER`, `ESC`, `BACKSPACE`, `TAB`, `SPACE`, `DEL`, `RIGHT`, `LEFT`,
`DOWN`, `UP`, `CTRL`, `SHIFT` (`SHIFT_L`), `SHIFT_R`, `ALT`, and the keys a USB keyboard has none of - `SYM`, `MIC`, `DOLLAR` (codes
0x100, 0x101, 0x102) - or a code (`0x2B`, `43`), or `-` for a place without a key.

The places not given are keys that do not exist: their events are dropped.
