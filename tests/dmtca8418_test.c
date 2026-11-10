#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmtca8418.h"
#include <string.h>

static dmtca8418_config_t config_of_t_deck(void)
{
    dmtca8418_config_t c;
    memset(&c, 0, sizeof(c));
    c.address = 0x34;
    c.rows = 4;
    c.cols = 10;
    c.mirror_cols = true;
    c.poll_ms = 10;
    c.keys[0][0] = DMDRVI_KEY_A + ('Q' - 'A');          /* the first key of the first row as the keyboard has it (left) */
    c.keys[0][9] = DMDRVI_KEY_A + ('P' - 'A');
    c.keys[2][1] = DMDRVI_KEY_A + ('Z' - 'A');
    return c;
}

static bool key(const char *name, uint16_t *code)
{
    return dmtca8418_parse_key(name, strlen(name), code);
}

DMOD_TEST_STEP(configuration_is_checked)
{
    dmtca8418_config_t c = config_of_t_deck();
    DMOD_TEST_EXPECT_TRUE(dmtca8418_validate_config(&c));
    DMOD_TEST_EXPECT_FALSE(dmtca8418_validate_config(NULL));
    c.address = 0x02;
    DMOD_TEST_EXPECT_FALSE(dmtca8418_validate_config(&c));         /* not an I2C address */
    c = config_of_t_deck();
    c.rows = 9;
    DMOD_TEST_EXPECT_FALSE(dmtca8418_validate_config(&c));         /* the chip has 8 rows */
    c = config_of_t_deck();
    c.cols = 11;
    DMOD_TEST_EXPECT_FALSE(dmtca8418_validate_config(&c));         /* ... and 10 columns */
    c = config_of_t_deck();
    c.poll_ms = 0;
    DMOD_TEST_EXPECT_FALSE(dmtca8418_validate_config(&c));
    c = config_of_t_deck();
    strcpy(c.layout, "compact");
    DMOD_TEST_EXPECT_TRUE(dmtca8418_validate_config(&c));          /* the layout is a name... */
    memset(c.layout, 'x', sizeof(c.layout));
    DMOD_TEST_EXPECT_FALSE(dmtca8418_validate_config(&c));         /* ... that ends */
}

DMOD_TEST_STEP(events_of_the_chip_are_decoded)
{
    dmtca8418_raw_event_t e;
    DMOD_TEST_EXPECT_FALSE(dmtca8418_decode_event(0x00, &e));      /* no event */
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x01, &e));        /* key 1: row 0, column 0 - released (the bit 7 is the press) */
    DMOD_TEST_EXPECT_TRUE(e.matrix && !e.pressed && e.row == 0 && e.col == 0);
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x81, &e));
    DMOD_TEST_EXPECT_TRUE(e.matrix && e.pressed && e.row == 0 && e.col == 0);
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 40, &e));   /* key 40: the last of 4 x 10 */
    DMOD_TEST_EXPECT_TRUE(e.pressed && e.row == 3 && e.col == 9);
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 15, &e));   /* key 15: row 1, column 4 */
    DMOD_TEST_EXPECT_TRUE(e.row == 1 && e.col == 4);
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 80, &e));   /* the last of the 8 x 10 the chip has */
    DMOD_TEST_EXPECT_TRUE(e.matrix && e.row == 7 && e.col == 9);
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 0x5B, &e)); /* an event of a GPIO: not a key */
    DMOD_TEST_EXPECT_FALSE(e.matrix);
}

DMOD_TEST_STEP(a_place_of_the_matrix_is_a_key)
{
    dmtca8418_config_t c = config_of_t_deck();
    dmtca8418_raw_event_t e;
    /* The chip counts the columns from the right: its column 9 is the first (left) key of the row */
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 10, &e));   /* key 10: row 0, the chip's column 9 */
    DMOD_TEST_EXPECT_EQ(dmtca8418_key_of(&c, &e), DMDRVI_KEY_A + ('Q' - 'A'));
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 1, &e));    /* key 1: the chip's column 0, the last key of the row */
    DMOD_TEST_EXPECT_EQ(dmtca8418_key_of(&c, &e), DMDRVI_KEY_A + ('P' - 'A'));
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 29, &e));   /* key 29: row 2, the chip's column 8: mirrored 1 */
    DMOD_TEST_EXPECT_EQ(dmtca8418_key_of(&c, &e), DMDRVI_KEY_A + ('Z' - 'A'));
    c.mirror_cols = false;                                           /* counted from the left: the places are as the chip says */
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 1, &e));
    DMOD_TEST_EXPECT_EQ(dmtca8418_key_of(&c, &e), DMDRVI_KEY_A + ('Q' - 'A'));
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 5, &e));    /* a place without a key */
    DMOD_TEST_EXPECT_EQ(dmtca8418_key_of(&c, &e), 0);
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 0x5B, &e)); /* not a matrix event */
    DMOD_TEST_EXPECT_EQ(dmtca8418_key_of(&c, &e), 0);
    DMOD_TEST_EXPECT_TRUE(dmtca8418_decode_event(0x80 | 41, &e));   /* a row the keyboard does not have */
    DMOD_TEST_EXPECT_EQ(dmtca8418_key_of(&c, &e), 0);
}

DMOD_TEST_STEP(keys_have_names)
{
    uint16_t code = 0xFFFF;
    DMOD_TEST_EXPECT_TRUE(key("Q", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_A + ('Q' - 'A'));
    DMOD_TEST_EXPECT_TRUE(key("1", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_1);
    DMOD_TEST_EXPECT_TRUE(key("0", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_0);
    DMOD_TEST_EXPECT_TRUE(key("ENTER", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_ENTER);
    DMOD_TEST_EXPECT_TRUE(key("BACKSPACE", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_BACKSPACE);
    DMOD_TEST_EXPECT_TRUE(key("SPACE", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_SPACE);
    DMOD_TEST_EXPECT_TRUE(key("SHIFT", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_LEFT_SHIFT);
    DMOD_TEST_EXPECT_TRUE(key("SHIFT_R", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_RIGHT_SHIFT);
    DMOD_TEST_EXPECT_TRUE(key("ALT", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_LEFT_ALT);
    DMOD_TEST_EXPECT_TRUE(key("SYM", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_SYM);                  /* the codes of dmdrvi, the same for every driver */
    DMOD_TEST_EXPECT_TRUE(code >= DMDRVI_KEY_VENDOR_BASE);
    DMOD_TEST_EXPECT_TRUE(key("MIC", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_MIC);
    DMOD_TEST_EXPECT_TRUE(key("DOLLAR", &code));
    DMOD_TEST_EXPECT_EQ(code, DMDRVI_KEY_DOLLAR);
    DMOD_TEST_EXPECT_TRUE(key("-", &code));
    DMOD_TEST_EXPECT_EQ(code, 0);                                    /* a place without a key */
    DMOD_TEST_EXPECT_TRUE(key("0x2B", &code));
    DMOD_TEST_EXPECT_EQ(code, 0x2B);
    DMOD_TEST_EXPECT_TRUE(key("43", &code));
    DMOD_TEST_EXPECT_EQ(code, 43);
    DMOD_TEST_EXPECT_FALSE(key("NOPE", &code));
    DMOD_TEST_EXPECT_FALSE(key("q", &code));                         /* the names are in capitals */
    DMOD_TEST_EXPECT_FALSE(key("0x", &code));
    DMOD_TEST_EXPECT_FALSE(key("0x10000", &code));                   /* a code is 16 bits */
    DMOD_TEST_EXPECT_FALSE(dmtca8418_parse_key("Q", 0, &code));
    DMOD_TEST_EXPECT_FALSE(dmtca8418_parse_key(NULL, 1, &code));
}
