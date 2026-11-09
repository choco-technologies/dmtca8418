# API reference

## The node (dmdrvi)

| Command | |
|---------|-|
| `DMDRVI_IOCTL_INPUT_GET_INFO` | Name `TCA8418`, type `DMDRVI_INPUT_TYPE_KEYBOARD`, `DMDRVI_INPUT_CAP_KEYS`, and `DMDRVI_INPUT_CAP_LIGHT` when there is a `backlight` friend |
| `DMDRVI_IOCTL_INPUT_KEY_READ` | The events of this open node (`dmdrvi_key_events_t`), up to `DMDRVI_KEY_MAX_EVENTS`; never blocks. `read()` is the same |
| `DMDRVI_IOCTL_INPUT_WAIT_EVENT` | Waits (polling the chip every `poll_ms`) until this open node has an event or the timeout ends (`-ETIMEDOUT`) |
| `DMDRVI_IOCTL_INPUT_SET_LIGHT` / `GET_LIGHT` | The light of the keys, on (any value above 0 gives 100) or off; `-ENOTSUP` without a `backlight` friend |
| `DMDRVI_IOCTL_INPUT_GET_STATE` | `-ENOTSUP` (a keyboard has events, not a state) |
| `dmtca8418_ioctl_cmd_get_config` | The `dmtca8418_config_t` the driver uses |

The events are `DMDRVI_KEY_ACTION_DOWN` / `_UP` with the code of the key; `DMDRVI_KEY_ACTION_OVERFLOW` when events were lost (the queue of
the node was full, or the FIFO of the chip overflowed): a key seen down may be up. See "Keyboards" in the dmdrvi documentation.

## The library (`dmtca8418.h`)

| Function | |
|----------|-|
| `dmtca8418_validate_config(config)` | Whether the address, the matrix and the polling period make sense |
| `dmtca8418_decode_event(byte, &event)` | A byte of the FIFO of the chip: `1..80` is the release of a key of the matrix (number = row * 10 + column + 1), with the bit 7 its press, `0x5B..0x72` GPIO events |
| `dmtca8418_key_of(config, &event)` | The code of the key of a place of the matrix (the mirroring of the columns applied), 0 for none |
| `dmtca8418_parse_key(text, length, &code)` | The code of a key written in a configuration |
