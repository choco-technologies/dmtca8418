#ifndef DMTCA8418_TYPES_H
#define DMTCA8418_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "dmdrvi_ioctl.h"

/** Rows and columns of the matrix the chip scans: R0..R7, C0..C9 */
#define DMTCA8418_MAX_ROWS           8U
#define DMTCA8418_MAX_COLS           10U

/** Key events the driver keeps for an open node (the one that does not fit makes the oldest go) */
#define DMTCA8418_QUEUE_SIZE         32U

/**
 * @brief Configuration of a keyboard (the ini section of the driver)
 *
 * `keys` tells which key every place of the matrix is: the code that DMDRVI_IOCTL_INPUT_KEY_READ reports (a USB HID usage, or DMDRVI_KEY_SYM, _MIC,
 * _DOLLAR), 0 for a place without a key.
 */
typedef struct
{
    uint8_t  address;                                           /**< 7-bit I2C address of the chip (0x34) */
    uint8_t  rows;                                              /**< Rows of the matrix, 1..DMTCA8418_MAX_ROWS */
    uint8_t  cols;                                              /**< Columns of the matrix, 1..DMTCA8418_MAX_COLS */
    bool     mirror_cols;                                       /**< The columns are counted from the other side */
    uint16_t poll_ms;                                           /**< Period of the polling of the chip while a client waits (WAIT_EVENT) */
    char     layout[DMDRVI_INPUT_LAYOUT_MAX];                   /**< The layout of the keys the keyboard names in its info ("compact"); empty: an ordinary keyboard */
    bool     debug;                                             /**< Log every event of the chip (row, column, key): to find out the keys of a keyboard */
    uint16_t keys[DMTCA8418_MAX_ROWS][DMTCA8418_MAX_COLS];      /**< The key of every place, [row][col] */
} dmtca8418_config_t;

/** A key event of the chip, as it reports it */
typedef struct
{
    bool     matrix;                                            /**< A key of the matrix (not an event of a GPIO) */
    bool     pressed;
    uint8_t  row;
    uint8_t  col;
} dmtca8418_raw_event_t;

/** Commands of the node, besides the generic input ones (DMDRVI_IOCTL_INPUT_*) */
typedef enum
{
    dmtca8418_ioctl_cmd_get_config = DMDRVI_IOCTL_CUSTOM_BASE,  /**< arg: dmtca8418_config_t* */
    dmtca8418_ioctl_cmd_max
} dmtca8418_ioctl_cmd_t;

#endif /* DMTCA8418_TYPES_H */
