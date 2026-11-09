#ifndef DMTCA8418_H
#define DMTCA8418_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "dmod_types.h"
#include "dmtca8418_defs.h"
#include "dmtca8418_types.h"

/**
 * @brief Check a configuration without touching hardware
 *
 * @return true when the address, the size of the matrix and the period of the polling make sense.
 */
dmod_dmtca8418_api(1.0, bool, _validate_config, ( const dmtca8418_config_t *config ));

/**
 * @brief Decode a key event of the chip (the byte read from KEY_EVENT_A..J)
 *
 * The chip reports 0 for no event; 1..80 is the key press of the matrix (number = row * 10 + column + 1), the same with the bit 7 set is
 * its release; 0x5B..0x72 (and with the bit 7) are the events of the GPIOs that are not keys.
 *
 * @param byte    The byte from the chip
 * @param event   Output: what it says
 *
 * @return true when `byte` is an event (not 0).
 */
dmod_dmtca8418_api(1.0, bool, _decode_event, ( uint8_t byte, dmtca8418_raw_event_t *event ));

/**
 * @brief The key of a place of the matrix
 *
 * @param config  The configuration
 * @param event   A matrix event (decoded)
 *
 * @return The code of the key (a USB HID usage, or one from DMDRVI_KEY_VENDOR_BASE), 0 when the place is out of the matrix or has no key.
 */
dmod_dmtca8418_api(1.0, uint16_t, _key_of, ( const dmtca8418_config_t *config, const dmtca8418_raw_event_t *event ));

/**
 * @brief The code of a key written in a configuration: a letter, a digit, a name (ENTER, SHIFT, SYM...) or a number
 *
 * @param text    The name
 * @param length  Characters in it
 * @param code    Output
 *
 * @return true when it is a key.
 */
dmod_dmtca8418_api(1.0, bool, _parse_key, ( const char *text, size_t length, uint16_t *code ));

#endif // DMTCA8418_H
