#define DMOD_ENABLE_REGISTRATION    ON
#include "dmod.h"
#include "dmtca8418.h"
#include "dmdrvi.h"
#include "dmini.h"
#include "dmosi.h"
#include "dmgpio_types.h"
#include "dmi2c_types.h"
#include <errno.h>
#include <string.h>

/*
 * dmtca8418: a keyboard with a TCA8418 (Texas Instruments: a keypad scanner for up to 8 rows and 10 columns, with a FIFO of 10 key events)
 * on I2C, as on the LILYGO T-Deck Pro (4 rows, 10 columns).
 *
 * The node follows the keyboard part of the generic input interface (DMDRVI_INPUT_TYPE_KEYBOARD, DMDRVI_IOCTL_INPUT_KEY_READ and
 * WAIT_EVENT) of dmdrvi: raw key events - which key went down or up, as a USB HID usage - and nothing of what the keys mean (the shift, alt
 * and sym layers, the layout of a language, repeating a key are the job of libinput). The place of every key is in the configuration.
 *
 * The chip is polled while a client waits (WAIT_EVENT) and when a client asks (KEY_READ): nothing runs in the background. Each open node has
 * a queue of its own of the events since it was opened; with no node open nothing is polled, and what the chip kept is thrown away when
 * the first one is opened - a reader never finds the keys of the minutes before.
 *
 * The wiring is done by dmdevfs: the sections of the keyboard, of its I2C bus (dmi2c) and of the backlight are in one friends_group, and
 * the roles of the friends (friend_role=...) tell this driver which node is which:
 *
 *   i2c         the dmi2c node the chip is on (required)
 *   backlight   the line of the light of the keys (optional: DMDRVI_INPUT_CAP_LIGHT, on or off - there is no dimming)
 *
 * The interrupt line of the chip is not used (the driver polls).
 */

/* Magic set to "T418" */
#define DMTCA8418_CONTEXT_MAGIC     0x54343138

#define REG_CFG                     0x01U
#define REG_INT_STAT                0x02U
#define REG_KEY_LCK_EC              0x03U       /* low nibble: events in the FIFO */
#define REG_KEY_EVENT_A             0x04U       /* reading it takes the oldest event out of the FIFO */
#define REG_KP_GPIO_1               0x1DU       /* which of R0..R7, C0..C7, C8..C9 are keypad pins (not GPIOs) */
#define REG_KP_GPIO_2               0x1EU
#define REG_KP_GPIO_3               0x1FU

#define INT_STAT_K_INT              0x01U       /* key events: written back, clears it */
#define INT_STAT_OVR_FLOW           0x08U       /* the FIFO overflowed: events were lost */
#define FIFO_SIZE                   10U
#define DEFAULT_ADDRESS             0x34U
#define START_TRIES                 3U
#define RETRY_MS                    2000U

typedef struct
{
    char   *path;                   /**< Node of the friend (NULL: not there) */
    void   *file;                   /**< Kept open once used */
} node_t;

/* An open node: the events since it was opened */
typedef struct handle
{
    struct handle      *next;
    dmdrvi_key_event_t  events[DMTCA8418_QUEUE_SIZE];
    uint32_t            first, count;
    bool                lost;       /**< Events went (the queue was full, or the chip overflowed): OVERFLOW is the next one */
} handle_t;

struct dmdrvi_context
{
    uint32_t              magic;
    dmtca8418_config_t    config;
    uint8_t               instance;
    dmosi_mutex_t         lock;
    node_t                i2c;
    node_t                backlight;
    char                 *backlight_role;
    bool                  started;          /**< The chip answered and was set up */
    uint64_t              next_try_ms;      /**< Not before this: the chip did not answer, it is not asked again at once */
    uint8_t               light;            /**< The light as it was set, percent */
    handle_t             *handles;          /**< The open nodes */
};

static bool is_valid_context(dmdrvi_context_t context)
{
    return context != NULL && context->magic == DMTCA8418_CONTEXT_MAGIC;
}

/* ---- API ---- */

dmod_dmtca8418_api_declaration(1.0, bool, _validate_config, ( const dmtca8418_config_t *config ))
{
    if (config == NULL || config->address < 0x08 || config->address > 0x77)
        return false;
    if (config->rows < 1 || config->rows > DMTCA8418_MAX_ROWS || config->cols < 1 || config->cols > DMTCA8418_MAX_COLS)
        return false;
    if (config->poll_ms < 1)
        return false;
    for (size_t i = 0; i < sizeof(config->layout); i++)         /* (a layout is a string: it ends) */
    {
        if (config->layout[i] == '\0')
            return true;
    }
    return false;
}

dmod_dmtca8418_api_declaration(1.0, bool, _decode_event, ( uint8_t byte, dmtca8418_raw_event_t *event ))
{
    if (event == NULL)
        return false;
    memset(event, 0, sizeof(*event));
    if (byte == 0)
        return false;
    event->pressed = (byte & 0x80U) != 0U;
    uint8_t number = (uint8_t)(byte & 0x7FU);
    if (number >= 1U && number <= DMTCA8418_MAX_ROWS * DMTCA8418_MAX_COLS)
    {
        event->matrix = true;
        event->row = (uint8_t)((number - 1U) / DMTCA8418_MAX_COLS);
        event->col = (uint8_t)((number - 1U) % DMTCA8418_MAX_COLS);
    }
    return true;
}

dmod_dmtca8418_api_declaration(1.0, uint16_t, _key_of, ( const dmtca8418_config_t *config, const dmtca8418_raw_event_t *event ))
{
    if (config == NULL || event == NULL || !event->matrix || event->row >= config->rows || event->col >= config->cols)
        return 0;
    uint8_t col = config->mirror_cols ? (uint8_t)(config->cols - 1U - event->col) : event->col;
    return config->keys[event->row][col];
}

/* ---- The names of the keys ---- */

static bool word_is(const char *text, size_t length, const char *word)
{
    return strlen(word) == length && strncmp(text, word, length) == 0;
}

static bool parse_number(const char *text, size_t length, uint32_t *value)
{
    uint32_t v = 0, base = 10;
    size_t i = 0;
    if (length > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
    {
        base = 16;
        i = 2;
    }
    if (i >= length)
        return false;
    for (; i < length; i++)
    {
        char c = text[i];
        uint32_t d = (c >= '0' && c <= '9') ? (uint32_t)(c - '0') :
                     (base == 16 && c >= 'a' && c <= 'f') ? (uint32_t)(c - 'a' + 10) :
                     (base == 16 && c >= 'A' && c <= 'F') ? (uint32_t)(c - 'A' + 10) : 99U;
        if (d >= base)
            return false;
        v = v * base + d;
    }
    *value = v;
    return true;
}

dmod_dmtca8418_api_declaration(1.0, bool, _parse_key, ( const char *text, size_t length, uint16_t *code ))
{
    uint32_t v;
    if (text == NULL || code == NULL || length == 0)
        return false;
    if (length == 1 && text[0] >= 'A' && text[0] <= 'Z')
        *code = (uint16_t)(DMDRVI_KEY_A + (text[0] - 'A'));
    else if (length == 1 && text[0] >= '1' && text[0] <= '9')
        *code = (uint16_t)(DMDRVI_KEY_1 + (text[0] - '1'));
    else if (length == 1 && text[0] == '0')
        *code = DMDRVI_KEY_0;
    else if (length == 1 && text[0] == '-')
        *code = 0;                                  /* a place without a key */
    else if (word_is(text, length, "ENTER"))
        *code = DMDRVI_KEY_ENTER;
    else if (word_is(text, length, "ESC"))
        *code = DMDRVI_KEY_ESCAPE;
    else if (word_is(text, length, "BACKSPACE"))
        *code = DMDRVI_KEY_BACKSPACE;
    else if (word_is(text, length, "TAB"))
        *code = DMDRVI_KEY_TAB;
    else if (word_is(text, length, "SPACE"))
        *code = DMDRVI_KEY_SPACE;
    else if (word_is(text, length, "DEL"))
        *code = DMDRVI_KEY_DELETE;
    else if (word_is(text, length, "RIGHT"))
        *code = DMDRVI_KEY_RIGHT;
    else if (word_is(text, length, "LEFT"))
        *code = DMDRVI_KEY_LEFT;
    else if (word_is(text, length, "DOWN"))
        *code = DMDRVI_KEY_DOWN;
    else if (word_is(text, length, "UP"))
        *code = DMDRVI_KEY_UP;
    else if (word_is(text, length, "CTRL"))
        *code = DMDRVI_KEY_LEFT_CTRL;
    else if (word_is(text, length, "SHIFT") || word_is(text, length, "SHIFT_L"))
        *code = DMDRVI_KEY_LEFT_SHIFT;
    else if (word_is(text, length, "SHIFT_R"))
        *code = DMDRVI_KEY_RIGHT_SHIFT;
    else if (word_is(text, length, "ALT"))
        *code = DMDRVI_KEY_LEFT_ALT;
    else if (word_is(text, length, "SYM"))
        *code = DMDRVI_KEY_SYM;
    else if (word_is(text, length, "MIC"))
        *code = DMDRVI_KEY_MIC;
    else if (word_is(text, length, "DOLLAR"))
        *code = DMDRVI_KEY_DOLLAR;
    else if (parse_number(text, length, &v) && v > 0 && v < 0x10000U)
        *code = (uint16_t)v;
    else
        return false;
    return true;
}

/* ---- Configuration ---- */

/* The places of a row: "Q W E R T ...", a name for each column (- for none) */
static int read_row(dmtca8418_config_t *c, uint8_t row, const char *text)
{
    uint8_t col = 0;
    size_t length = strlen(text), from = 0;
    for (size_t i = 0; i <= length; i++)
    {
        if (i < length && text[i] != ' ')
            continue;
        if (i > from)
        {
            uint16_t code;
            if (col >= c->cols || !dmtca8418_parse_key(text + from, i - from, &code))
                return -EINVAL;
            c->keys[row][col++] = code;
        }
        from = i + 1U;
    }
    return 0;
}

static bool string_to_switch(const char *text, bool fallback)
{
    if (text == NULL)
        return fallback;
    if (strcmp(text, "on") == 0 || strcmp(text, "yes") == 0 || strcmp(text, "true") == 0 || strcmp(text, "1") == 0)
        return true;
    if (strcmp(text, "off") == 0 || strcmp(text, "no") == 0 || strcmp(text, "false") == 0 || strcmp(text, "0") == 0)
        return false;
    return fallback;
}

static int read_config(dmdrvi_context_t context, dmini_context_t ini)
{
    dmtca8418_config_t *c = &context->config;

    context->instance = (uint8_t)dmini_get_int(ini, NULL, "instance", 0);
    c->address      = (uint8_t)dmini_get_int(ini, NULL, "address", DEFAULT_ADDRESS);
    c->rows         = (uint8_t)dmini_get_int(ini, NULL, "rows", 4);
    c->cols         = (uint8_t)dmini_get_int(ini, NULL, "cols", 10);
    c->mirror_cols  = string_to_switch(dmini_get_string(ini, NULL, "mirror_cols", NULL), false);
    c->poll_ms      = (uint16_t)dmini_get_int(ini, NULL, "poll_ms", 10);
    c->debug        = string_to_switch(dmini_get_string(ini, NULL, "debug", NULL), false);
    const char *layout = dmini_get_string(ini, NULL, "layout", "");
    if (strlen(layout) >= sizeof(c->layout))
    {
        DMOD_LOG_ERROR("Keyboard: the name of the layout %s is too long (%u at most)\n", layout, (unsigned)sizeof(c->layout) - 1U);
        return -EINVAL;
    }
    strcpy(c->layout, layout);
    context->backlight_role = Dmod_StrDup(dmini_get_string(ini, NULL, "backlight_role", "backlight"));
    if (context->backlight_role == NULL)
        return -ENOMEM;
    if (!dmtca8418_validate_config(c))
    {
        DMOD_LOG_ERROR("Invalid keyboard configuration (%ux%u matrix, address 0x%02X)\n", (unsigned)c->rows, (unsigned)c->cols,
                       c->address);
        return -EINVAL;
    }
    for (uint8_t r = 0; r < c->rows; r++)
    {
        char name[8] = "row0";
        name[3] = (char)('0' + r);
        const char *text = dmini_get_string(ini, NULL, name, NULL);
        if (text != NULL && read_row(c, r, text) != 0)
        {
            DMOD_LOG_ERROR("Keyboard: %s is not a row of %u keys (%s)\n", name, (unsigned)c->cols, text);
            return -EINVAL;
        }
    }
    return 0;
}

/* ---- The nodes of the friends ---- */

static void node_close(node_t *node)
{
    if (node->file != NULL)
        Dmod_FileClose(node->file);
    node->file = NULL;
}

static void node_forget(node_t *node)
{
    node_close(node);
    Dmod_Free(node->path);
    node->path = NULL;
}

static void *node_file(node_t *node)
{
    if (node->file == NULL && node->path != NULL)
        node->file = Dmod_FileOpen(node->path, "r+");
    return node->file;
}

static int gpio_set(node_t *node, bool high)
{
    dmgpio_pins_state_t state = high ? dmgpio_pins_state_all_high : dmgpio_pins_state_all_low;
    void *file = node_file(node);
    if (file == NULL)
        return -ENODEV;
    if (Dmod_Ioctl(file, dmgpio_ioctl_cmd_set_pins_state, &state) != 0)
    {
        node_close(node);
        return -EIO;
    }
    return 0;
}

/* ---- The chip: 8-bit registers ---- */

static int i2c_transfer(dmdrvi_context_t context, dmi2c_message_t *messages, size_t count)
{
    void *file = node_file(&context->i2c);
    dmi2c_transfer_t transfer = { messages, count };
    if (file == NULL)
        return -ENODEV;
    return Dmod_Ioctl(file, dmi2c_ioctl_cmd_transfer, &transfer);
}

static int reg_read(dmdrvi_context_t context, uint8_t reg, uint8_t *value)
{
    dmi2c_message_t messages[2] = {
        { .address = context->config.address, .read = false, .data = &reg,  .size = 1 },
        { .address = context->config.address, .read = true,  .data = value, .size = 1 },
    };
    return i2c_transfer(context, messages, 2);
}

static int reg_write(dmdrvi_context_t context, uint8_t reg, uint8_t value)
{
    uint8_t data[2] = { reg, value };
    dmi2c_message_t message = { .address = context->config.address, .read = false, .data = data, .size = sizeof(data) };
    return i2c_transfer(context, &message, 1);
}

/* Throw away what the chip kept: the events, and what it says about them */
static void chip_flush(dmdrvi_context_t context)
{
    uint8_t event = 0;
    for (unsigned i = 0; i < FIFO_SIZE + 2U; i++)
    {
        if (reg_read(context, REG_KEY_EVENT_A, &event) != 0 || event == 0)
            break;
    }
    (void)reg_write(context, REG_INT_STAT, INT_STAT_K_INT | INT_STAT_OVR_FLOW);
}

/* The rows and the columns the keyboard has are keypad pins, the others stay GPIOs (inputs); no interrupts: the chip is polled */
static int chip_setup(dmdrvi_context_t context)
{
    const dmtca8418_config_t *c = &context->config;
    uint8_t cfg = 0;
    int ret = reg_read(context, REG_CFG, &cfg);                 /* does it answer? */
    if (ret != 0)
        return ret;
    uint8_t rows = (uint8_t)((1U << c->rows) - 1U);
    uint8_t cols_low = (uint8_t)(c->cols >= 8U ? 0xFFU : (1U << c->cols) - 1U);
    uint8_t cols_high = (uint8_t)(c->cols > 8U ? (1U << (c->cols - 8U)) - 1U : 0U);
    ret = reg_write(context, REG_KP_GPIO_1, rows);
    if (ret == 0)
        ret = reg_write(context, REG_KP_GPIO_2, cols_low);
    if (ret == 0)
        ret = reg_write(context, REG_KP_GPIO_3, cols_high);
    if (ret == 0)
        chip_flush(context);
    return ret;
}

/* Set the chip up (the lock is held). It is done when a client first asks for events - in the thread of the client, not in the callback that
 * tells the driver about its friends (the thread of dmdevfs, while the system is starting) - and, when the chip does not answer, not more
 * often than every RETRY_MS. */
static void start_locked(dmdrvi_context_t context)
{
    if (context->started || context->i2c.path == NULL)
        return;
    uint64_t now = Dmod_GetUptime();
    if (now < context->next_try_ms)
        return;
    int ret = -ENODEV;
    for (unsigned try = 0; ret != 0 && try < START_TRIES; try++)
        ret = chip_setup(context);
    if (ret == 0)
    {
        context->started = true;
        DMOD_LOG_INFO("TCA8418 keyboard %ux%u started\n", (unsigned)context->config.rows, (unsigned)context->config.cols);
    }
    else
    {
        context->next_try_ms = now + RETRY_MS;
        DMOD_LOG_ERROR("TCA8418: no answer at 0x%02X (%d)\n", context->config.address, ret);
    }
}

/* ---- The queues of the open nodes ---- */

/* An event for every open node; a full queue loses its oldest event (and the node is told) */
static void queue_event(dmdrvi_context_t context, uint16_t code, uint8_t action, uint32_t time_ms)
{
    for (handle_t *h = context->handles; h != NULL; h = h->next)
    {
        if (h->count == DMTCA8418_QUEUE_SIZE)
        {
            h->first = (h->first + 1U) % DMTCA8418_QUEUE_SIZE;
            h->count--;
            h->lost = true;
        }
        dmdrvi_key_event_t *e = &h->events[(h->first + h->count) % DMTCA8418_QUEUE_SIZE];
        e->time_ms = time_ms;
        e->code = code;
        e->action = action;
        e->reserved = 0;
        h->count++;
    }
}

static void lose_events(dmdrvi_context_t context)
{
    for (handle_t *h = context->handles; h != NULL; h = h->next)
        h->lost = true;
}

/* Take the events of the chip (the lock is held) and give them to the open nodes */
static int poll_once(dmdrvi_context_t context)
{
    uint8_t stat = 0, count = 0;
    start_locked(context);
    if (!context->started)
        return -ENODEV;
    int ret = reg_read(context, REG_INT_STAT, &stat);
    if (ret == 0)
        ret = reg_read(context, REG_KEY_LCK_EC, &count);
    if (ret != 0)
        return ret;
    if ((stat & INT_STAT_OVR_FLOW) != 0U)
        lose_events(context);                           /* the chip lost some: a key seen down may be up */
    count &= 0x0FU;
    uint32_t now = (uint32_t)Dmod_GetUptime();
    for (uint8_t i = 0; i < count; i++)
    {
        uint8_t byte = 0;
        dmtca8418_raw_event_t raw;
        if (reg_read(context, REG_KEY_EVENT_A, &byte) != 0)
            return -EIO;
        if (!dmtca8418_decode_event(byte, &raw) || !raw.matrix)
            continue;
        uint16_t key = dmtca8418_key_of(&context->config, &raw);
        if (context->config.debug)
            DMOD_LOG_INFO("TCA8418: R%u C%u %s (chip %02X) key 0x%X\n", (unsigned)raw.row, (unsigned)raw.col,
                          raw.pressed ? "down" : "up", (unsigned)byte, (unsigned)key);
        if (key != 0)
            queue_event(context, key, raw.pressed ? DMDRVI_KEY_ACTION_DOWN : DMDRVI_KEY_ACTION_UP, now);
    }
    if (stat != 0U)
        (void)reg_write(context, REG_INT_STAT, stat);   /* (written back it is cleared) */
    return 0;
}

static bool pending(const handle_t *h)
{
    return h->count > 0 || h->lost;
}

/* The events of a node for a client: OVERFLOW first when some were lost */
static void take_events(handle_t *h, dmdrvi_key_events_t *out)
{
    memset(out, 0, sizeof(*out));
    if (h->lost)
    {
        out->events[out->count].time_ms = (uint32_t)Dmod_GetUptime();
        out->events[out->count].action = DMDRVI_KEY_ACTION_OVERFLOW;
        out->count++;
        h->lost = false;
    }
    while (h->count > 0 && out->count < DMDRVI_KEY_MAX_EVENTS)
    {
        out->events[out->count++] = h->events[h->first];
        h->first = (h->first + 1U) % DMTCA8418_QUEUE_SIZE;
        h->count--;
    }
}

static int key_read(dmdrvi_context_t context, handle_t *h, dmdrvi_key_events_t *out)
{
    dmosi_mutex_lock(context->lock);
    int ret = poll_once(context);
    if (ret == 0)
        take_events(h, out);
    dmosi_mutex_unlock(context->lock);
    return ret;
}

static int wait_event(dmdrvi_context_t context, handle_t *h, const uint32_t *timeout_ms)
{
    uint64_t start = Dmod_GetUptime();

    for (;;)
    {
        dmosi_mutex_lock(context->lock);
        int ret = poll_once(context);
        bool ready = pending(h);
        dmosi_mutex_unlock(context->lock);
        if (ret != 0)
            return ret;
        if (ready)
            return 0;
        if (timeout_ms != NULL && Dmod_GetUptime() - start >= *timeout_ms)
            return -ETIMEDOUT;
        dmosi_thread_sleep(context->config.poll_ms);
    }
}

/* ---- The light ---- */

static int set_light(dmdrvi_context_t context, uint8_t percent)
{
    if (percent > 100U)
        return -EINVAL;
    if (context->backlight.path == NULL)
        return -ENOTSUP;
    int ret = gpio_set(&context->backlight, percent != 0U);
    if (ret == 0)
        context->light = percent != 0U ? 100U : 0U;     /* on or off: there is no dimming */
    return ret;
}

/* ---- DMOD lifecycle ---- */

int dmod_init(const Dmod_Config_t *Config)
{
    (void)Config;
    DMOD_LOG_INFO("DMTCA8418 module initialized\n");
    return 0;
}

int dmod_deinit(void)
{
    DMOD_LOG_INFO("DMTCA8418 module deinitialized\n");
    return 0;
}

/* ---- DMDRVI interface ---- */

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, dmdrvi_context_t, _create, ( dmini_context_t config, dmdrvi_dev_num_t* dev_num ))
{
    if (config == NULL || dev_num == NULL)
        return NULL;
    dmdrvi_context_t context = Dmod_Malloc(sizeof(struct dmdrvi_context));
    if (context == NULL)
        return NULL;
    memset(context, 0, sizeof(*context));
    context->magic = DMTCA8418_CONTEXT_MAGIC;

    if (read_config(context, config) != 0)
        goto fail;
    context->lock = dmosi_mutex_create(false);
    if (context->lock == NULL)
    {
        DMOD_LOG_ERROR("TCA8418: out of memory\n");
        goto fail;
    }
    dev_num->flags = DMDRVI_NUM_MAJOR;
    dev_num->major = (dmdrvi_dev_id_t)context->instance;
    return context;

fail:
    Dmod_Free(context->backlight_role);
    context->magic = 0;
    Dmod_Free(context);
    return NULL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, void, _free, ( dmdrvi_context_t context ))
{
    if (!is_valid_context(context))
        return;
    /* The nodes of the friends are not touched: dmdevfs tears them down before this call, in reverse order of the sections */
    while (context->handles != NULL)
    {
        handle_t *h = context->handles;
        context->handles = h->next;
        Dmod_Free(h);
    }
    Dmod_Free(context->i2c.path);
    Dmod_Free(context->backlight.path);
    Dmod_Free(context->backlight_role);
    dmosi_mutex_destroy(context->lock);
    context->magic = 0;
    Dmod_Free(context);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, void, _friend_changed, ( dmdrvi_context_t context, const dmdrvi_friend_info_t* info ))
{
    if (!is_valid_context(context) || info == NULL || info->friend_role == NULL)
        return;
    node_t *node = strcmp(info->friend_role, "i2c") == 0 ? &context->i2c
                 : strcmp(info->friend_role, context->backlight_role) == 0 ? &context->backlight : NULL;
    if (node == NULL)
        return;
    node_forget(node);
    if (node == &context->i2c)
        context->started = false;                       /* a new bus: the chip is set up again */
    if (info->state != dmdrvi_dev_state_ready || info->node_path == NULL)
        return;
    node->path = Dmod_StrDup(info->node_path);
    if (node->path == NULL)
        DMOD_LOG_ERROR("TCA8418: out of memory for the path of a friend\n");
    /* (the chip is set up when a client asks for events: see start_locked()) */
}

/* A node: its queue starts empty, and what the chip kept before is thrown away (for the first one) */
dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, void*, _open, ( dmdrvi_context_t context, int flags, const dmdrvi_dev_num_t* dev_num ))
{
    if (!is_valid_context(context))
        return NULL;
    handle_t *h = Dmod_Malloc(sizeof(*h));
    if (h == NULL)
        return NULL;
    memset(h, 0, sizeof(*h));
    dmosi_mutex_lock(context->lock);
    if (context->handles == NULL && context->started)
        chip_flush(context);
    h->next = context->handles;
    context->handles = h;
    dmosi_mutex_unlock(context->lock);
    return h;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, void, _close, ( dmdrvi_context_t context, void* handle ))
{
    if (!is_valid_context(context) || handle == NULL)
        return;
    dmosi_mutex_lock(context->lock);
    for (handle_t **p = &context->handles; *p != NULL; p = &(*p)->next)
    {
        if (*p == handle)
        {
            *p = (*p)->next;
            Dmod_Free(handle);
            break;
        }
    }
    dmosi_mutex_unlock(context->lock);
}

/* read(): the events of KEY_READ, one dmdrvi_key_events_t */
dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, dmdrvi_ssize_t, _read, ( dmdrvi_context_t context, void* handle, void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    if (!is_valid_context(context) || handle == NULL || buffer == NULL || size < sizeof(dmdrvi_key_events_t))
        return -EINVAL;
    int ret = key_read(context, (handle_t *)handle, (dmdrvi_key_events_t *)buffer);
    return ret != 0 ? ret : (dmdrvi_ssize_t)sizeof(dmdrvi_key_events_t);
}

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, dmdrvi_ssize_t, _write, ( dmdrvi_context_t context, void* handle, const void* buffer, size_t size, dmdrvi_offset_t offset ))
{
    return -ENOTSUP;
}

static int get_info(dmdrvi_context_t context, dmdrvi_input_info_t *info)
{
    if (info == NULL)
        return -EINVAL;
    memset(info, 0, sizeof(*info));
    memcpy(info->name, "TCA8418", 7);
    info->type = DMDRVI_INPUT_TYPE_KEYBOARD;
    info->capabilities = DMDRVI_INPUT_CAP_KEYS | (context->backlight.path != NULL ? DMDRVI_INPUT_CAP_LIGHT : 0U);
    /* The layout of the keys and where its file may be (cfg/layouts/dmtca8418/): libinput looks there first */
    strcpy(info->layout, context->config.layout);
    strcpy(info->module, "dmtca8418");
    return 0;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, int, _ioctl, ( dmdrvi_context_t context, void* handle, int command, void* arg ))
{
    if (!is_valid_context(context))
        return -EINVAL;

    switch (command)
    {
        case DMDRVI_IOCTL_INPUT_GET_INFO:
            return get_info(context, (dmdrvi_input_info_t *)arg);
        case DMDRVI_IOCTL_INPUT_GET_STATE:
            return -ENOTSUP;                            /* a keyboard has events, not a state */
        case DMDRVI_IOCTL_INPUT_KEY_READ:
            return (arg == NULL || handle == NULL) ? -EINVAL : key_read(context, (handle_t *)handle, (dmdrvi_key_events_t *)arg);
        case DMDRVI_IOCTL_INPUT_WAIT_EVENT:
            return handle == NULL ? -EINVAL : wait_event(context, (handle_t *)handle, (const uint32_t *)arg);
        case DMDRVI_IOCTL_INPUT_SET_LIGHT:
            return arg == NULL ? -EINVAL : set_light(context, *(const uint8_t *)arg);
        case DMDRVI_IOCTL_INPUT_GET_LIGHT:
            if (arg == NULL)
                return -EINVAL;
            *(uint8_t *)arg = context->light;
            return context->backlight.path != NULL ? 0 : -ENOTSUP;
        case dmtca8418_ioctl_cmd_get_config:
            if (arg == NULL)
                return -EINVAL;
            *(dmtca8418_config_t *)arg = context->config;
            return 0;
        default:
            return -ENOTTY;
    }
}

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, int, _flush, ( dmdrvi_context_t context, void* handle ))
{
    return is_valid_context(context) ? 0 : -EINVAL;
}

dmod_dmdrvi_dif_api_declaration(2.0, dmtca8418, int, _stat, ( dmdrvi_context_t context, const char* path, dmdrvi_stat_t* stat ))
{
    if (!is_valid_context(context) || stat == NULL)
        return -EINVAL;
    stat->size = (dmdrvi_size_t)sizeof(dmdrvi_key_events_t);
    stat->mode = 0444;
    return 0;
}
