# dmtca8418

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmtca8418/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmtca8418/actions/workflows/ci.yml)

The keyboard of the LILYGO T-Deck Pro: a driver for the **TCA8418** (Texas Instruments), a keypad scanner for up to 8 rows and 10
columns with a FIFO of 10 key events, on I2C.

The node follows the keyboard part of the generic input interface of dmdrvi (`DMDRVI_INPUT_TYPE_KEYBOARD`): **raw key events** - which
key went down or up, as a USB HID usage (or a code from `DMDRVI_KEY_VENDOR_BASE` for the keys a USB keyboard has none of: Sym, the
microphone key, `$`) - and nothing of what the keys mean. Shift, alt and sym layers, the layout of a language and repeating a held key
are libinput's ([dminput](https://github.com/choco-technologies/dminput)).

```
TCA8418 --I2C--> dmtca8418 (/dev/keyboard) --> inputd --> libinput --> dmview, ...
```

- **Events, per open node.** `DMDRVI_IOCTL_INPUT_KEY_READ` / `read()` take the events since the node was opened, `WAIT_EVENT` waits for
  them. Each open node has a queue of its own (32 events, the oldest goes when it is full and the next event is `OVERFLOW`); what the chip
  kept before the first node was opened is thrown away.
- **Polled.** The chip is read while a client waits and when it asks; nothing runs in the background (the interrupt line is not used).
- **The places of the keys** are in the configuration (`row0=Q W E R T Y U I O P`), so another keyboard on the chip is another file.
- **The light of the keys** (`DMDRVI_IOCTL_INPUT_SET_LIGHT`) on or off, through a `backlight` friend (dmgpio).

See [docs/configuration.md](docs/configuration.md) for the configuration and [docs/api-reference.md](docs/api-reference.md) for the API.
The configuration of the T-Deck Pro is [configs/board/t-deck-pro/keyboard.ini](configs/board/t-deck-pro/keyboard.ini).
