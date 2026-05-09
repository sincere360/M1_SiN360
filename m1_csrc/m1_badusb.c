/* See COPYING.txt for license details. */

/*
*
* m1_badusb.c
*
* BadUSB / Ducky-style script runner
*
* M1 Project
*
*/

/*************************** I N C L U D E S **********************************/

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <ctype.h>
#include "stm32h5xx_hal.h"
#include "main.h"
#include "m1_badusb.h"
#include "m1_display.h"
#include "m1_storage.h"
#include "m1_file_util.h"
#include "m1_usb_cdc_msc.h"
#include "m1_esp32_cmd.h"
#include "m1_esp32_hal.h"
#include "m1_bt.h"
#include "usbd_hid.h"
#include "ff.h"

/*************************** D E F I N E S ************************************/

#define BADUSB_SCRIPT_DIR          "badusb"
#define BADUSB_LINE_MAX            192U
#define BADUSB_DEFAULT_DELAY_MS    0U
#define BADUSB_KEY_DELAY_MS        25U
#define BADUSB_REPORT_TIMEOUT_MS   250U
#define BADUSB_REPORT_LEN          8U

#define HID_MOD_LCTRL              0x01U
#define HID_MOD_LSHIFT             0x02U
#define HID_MOD_LALT               0x04U
#define HID_MOD_LGUI               0x08U

typedef enum {
    BADUSB_TRANSPORT_USB = 0,
    BADUSB_TRANSPORT_BLE = 1,
} badusb_transport_t;

/***************************** V A R I A B L E S ******************************/

static uint32_t badusb_default_delay_ms = BADUSB_DEFAULT_DELAY_MS;
static badusb_transport_t badusb_transport = BADUSB_TRANSPORT_USB;
static int badusb_ble_last_rc = 0;
static uint8_t badusb_ble_last_status = RESP_OK;
static uint8_t badusb_ble_session_id = 0;

/********************* F U N C T I O N   P R O T O T Y P E S ******************/

static bool badusb_run_file(const char *path, badusb_transport_t transport);
static bool badusb_execute_line(char *line, uint32_t *line_no);
static bool badusb_send_key(uint8_t modifier, uint8_t keycode);
static bool badusb_send_text(const char *text);
static bool badusb_usb_send_report(uint8_t *report, uint16_t len);
static bool badusb_delay_cancelable(uint32_t delay_ms);
static bool badusb_user_cancelled(void);
static bool badusb_usb_ready(void);
static bool badusb_ble_start(const char *name);
static bool badusb_ble_stop(void);
static bool badusb_ble_ready(void);
static bool badusb_ble_send_report(uint8_t *report, uint16_t len);
static void badusb_ble_ensure_esp32_ready(void);
static void badusb_ble_make_session_name(char *out, size_t out_size);
static void badusb_status_draw(const char *title, const char *line1, const char *line2, const char *line3);
static void badusb_confirm_draw(const char *title, const char *name);

/*************** F U N C T I O N   I M P L E M E N T A T I O N ****************/

static void badusb_trim(char *s)
{
    size_t len;

    if (!s) return;
    len = strlen(s);
    while (len > 0 && (s[len - 1] == '\r' || s[len - 1] == '\n' || s[len - 1] == ' ' || s[len - 1] == '\t'))
    {
        s[len - 1] = '\0';
        len--;
    }
    while (*s == ' ' || *s == '\t')
    {
        memmove(s, s + 1, strlen(s));
    }
}

static bool badusb_starts_with_word(const char *line, const char *word)
{
    size_t len;

    if (!line || !word) return false;
    len = strlen(word);
    if (strncasecmp(line, word, len) != 0) return false;
    return line[len] == '\0' || line[len] == ' ' || line[len] == '\t';
}

static char *badusb_next_token(char **cursor)
{
    char *start;

    if (!cursor || !*cursor) return NULL;
    while (**cursor == ' ' || **cursor == '\t') (*cursor)++;
    if (**cursor == '\0') return NULL;

    start = *cursor;
    while (**cursor && **cursor != ' ' && **cursor != '\t') (*cursor)++;
    if (**cursor)
    {
        **cursor = '\0';
        (*cursor)++;
    }
    return start;
}

static bool badusb_ascii_to_hid(char c, uint8_t *modifier, uint8_t *keycode)
{
    *modifier = 0;
    *keycode = 0;

    if (c >= 'a' && c <= 'z')
    {
        *keycode = (uint8_t)(0x04 + (c - 'a'));
        return true;
    }
    if (c >= 'A' && c <= 'Z')
    {
        *modifier = HID_MOD_LSHIFT;
        *keycode = (uint8_t)(0x04 + (c - 'A'));
        return true;
    }
    if (c >= '1' && c <= '9')
    {
        *keycode = (uint8_t)(0x1E + (c - '1'));
        return true;
    }
    if (c == '0')
    {
        *keycode = 0x27;
        return true;
    }

    switch (c)
    {
        case ' ': *keycode = 0x2C; return true;
        case '\n': *keycode = 0x28; return true;
        case '!': *modifier = HID_MOD_LSHIFT; *keycode = 0x1E; return true;
        case '@': *modifier = HID_MOD_LSHIFT; *keycode = 0x1F; return true;
        case '#': *modifier = HID_MOD_LSHIFT; *keycode = 0x20; return true;
        case '$': *modifier = HID_MOD_LSHIFT; *keycode = 0x21; return true;
        case '%': *modifier = HID_MOD_LSHIFT; *keycode = 0x22; return true;
        case '^': *modifier = HID_MOD_LSHIFT; *keycode = 0x23; return true;
        case '&': *modifier = HID_MOD_LSHIFT; *keycode = 0x24; return true;
        case '*': *modifier = HID_MOD_LSHIFT; *keycode = 0x25; return true;
        case '(': *modifier = HID_MOD_LSHIFT; *keycode = 0x26; return true;
        case ')': *modifier = HID_MOD_LSHIFT; *keycode = 0x27; return true;
        case '\r': return true;
        case '-': *keycode = 0x2D; return true;
        case '_': *modifier = HID_MOD_LSHIFT; *keycode = 0x2D; return true;
        case '=': *keycode = 0x2E; return true;
        case '+': *modifier = HID_MOD_LSHIFT; *keycode = 0x2E; return true;
        case '[': *keycode = 0x2F; return true;
        case '{': *modifier = HID_MOD_LSHIFT; *keycode = 0x2F; return true;
        case ']': *keycode = 0x30; return true;
        case '}': *modifier = HID_MOD_LSHIFT; *keycode = 0x30; return true;
        case '\\': *keycode = 0x31; return true;
        case '|': *modifier = HID_MOD_LSHIFT; *keycode = 0x31; return true;
        case ';': *keycode = 0x33; return true;
        case ':': *modifier = HID_MOD_LSHIFT; *keycode = 0x33; return true;
        case '\'': *keycode = 0x34; return true;
        case '"': *modifier = HID_MOD_LSHIFT; *keycode = 0x34; return true;
        case '`': *keycode = 0x35; return true;
        case '~': *modifier = HID_MOD_LSHIFT; *keycode = 0x35; return true;
        case ',': *keycode = 0x36; return true;
        case '<': *modifier = HID_MOD_LSHIFT; *keycode = 0x36; return true;
        case '.': *keycode = 0x37; return true;
        case '>': *modifier = HID_MOD_LSHIFT; *keycode = 0x37; return true;
        case '/': *keycode = 0x38; return true;
        case '?': *modifier = HID_MOD_LSHIFT; *keycode = 0x38; return true;
        default: return false;
    }
}

static bool badusb_named_key(const char *token, uint8_t *modifier, uint8_t *keycode)
{
    int fn;

    *modifier = 0;
    *keycode = 0;
    if (!token || token[0] == '\0') return false;

    if (strcasecmp(token, "ENTER") == 0) *keycode = 0x28;
    else if (strcasecmp(token, "RETURN") == 0) *keycode = 0x28;
    else if (strcasecmp(token, "ESC") == 0 || strcasecmp(token, "ESCAPE") == 0) *keycode = 0x29;
    else if (strcasecmp(token, "BACKSPACE") == 0 || strcasecmp(token, "BKSP") == 0) *keycode = 0x2A;
    else if (strcasecmp(token, "TAB") == 0) *keycode = 0x2B;
    else if (strcasecmp(token, "SPACE") == 0) *keycode = 0x2C;
    else if (strcasecmp(token, "CAPSLOCK") == 0 || strcasecmp(token, "CAPS") == 0) *keycode = 0x39;
    else if (strcasecmp(token, "PRINTSCREEN") == 0) *keycode = 0x46;
    else if (strcasecmp(token, "SCROLLLOCK") == 0) *keycode = 0x47;
    else if (strcasecmp(token, "PAUSE") == 0) *keycode = 0x48;
    else if (strcasecmp(token, "INSERT") == 0) *keycode = 0x49;
    else if (strcasecmp(token, "HOME") == 0) *keycode = 0x4A;
    else if (strcasecmp(token, "PAGEUP") == 0) *keycode = 0x4B;
    else if (strcasecmp(token, "DELETE") == 0 || strcasecmp(token, "DEL") == 0) *keycode = 0x4C;
    else if (strcasecmp(token, "END") == 0) *keycode = 0x4D;
    else if (strcasecmp(token, "PAGEDOWN") == 0) *keycode = 0x4E;
    else if (strcasecmp(token, "RIGHT") == 0 || strcasecmp(token, "RIGHTARROW") == 0) *keycode = 0x4F;
    else if (strcasecmp(token, "LEFT") == 0 || strcasecmp(token, "LEFTARROW") == 0) *keycode = 0x50;
    else if (strcasecmp(token, "DOWN") == 0 || strcasecmp(token, "DOWNARROW") == 0) *keycode = 0x51;
    else if (strcasecmp(token, "UP") == 0 || strcasecmp(token, "UPARROW") == 0) *keycode = 0x52;
    else if ((token[0] == 'F' || token[0] == 'f') && token[1] >= '1' && token[1] <= '9')
    {
        fn = atoi(&token[1]);
        if (fn >= 1 && fn <= 12)
            *keycode = (uint8_t)(0x3A + fn - 1);
    }
    else if (strlen(token) == 1)
    {
        return badusb_ascii_to_hid(token[0], modifier, keycode);
    }

    return *keycode != 0;
}

static bool badusb_send_report(uint8_t modifier, uint8_t keycode)
{
    uint8_t report[BADUSB_REPORT_LEN] = {0};
    uint8_t empty[BADUSB_REPORT_LEN] = {0};

    report[0] = modifier;
    report[2] = keycode;

    if (badusb_transport == BADUSB_TRANSPORT_BLE)
    {
        if (!badusb_ble_send_report(report, sizeof(report)))
            return false;
    }
    else if (!badusb_usb_send_report(report, sizeof(report)))
    {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(BADUSB_KEY_DELAY_MS));
    if (badusb_transport == BADUSB_TRANSPORT_BLE)
    {
        if (!badusb_ble_send_report(empty, sizeof(empty)))
            return false;
    }
    else if (!badusb_usb_send_report(empty, sizeof(empty)))
    {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(BADUSB_KEY_DELAY_MS));
    return true;
}

static bool badusb_usb_send_report(uint8_t *report, uint16_t len)
{
    uint32_t start = HAL_GetTick();
    uint8_t ret;

    do
    {
        ret = USBD_HID_SendReport(&hUsbDeviceFS, report, len);
        if (ret == (uint8_t)USBD_OK)
            return true;
        if (ret != (uint8_t)USBD_BUSY)
            return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    } while ((HAL_GetTick() - start) < BADUSB_REPORT_TIMEOUT_MS);

    return false;
}

static bool badusb_send_key(uint8_t modifier, uint8_t keycode)
{
    if (keycode == 0) return true;
    return badusb_send_report(modifier, keycode);
}

static bool badusb_send_text(const char *text)
{
    uint8_t mod;
    uint8_t key;

    if (!text) return false;
    while (*text)
    {
        if (badusb_user_cancelled()) return false;
        if (badusb_ascii_to_hid(*text, &mod, &key) && key != 0)
        {
            if (!badusb_send_key(mod, key)) return false;
        }
        text++;
    }
    return true;
}

static uint8_t badusb_modifier_from_token(const char *token)
{
    if (!token) return 0;
    if (strcasecmp(token, "CTRL") == 0 || strcasecmp(token, "CONTROL") == 0) return HID_MOD_LCTRL;
    if (strcasecmp(token, "SHIFT") == 0) return HID_MOD_LSHIFT;
    if (strcasecmp(token, "ALT") == 0) return HID_MOD_LALT;
    if (strcasecmp(token, "GUI") == 0 || strcasecmp(token, "WINDOWS") == 0 || strcasecmp(token, "WIN") == 0 || strcasecmp(token, "COMMAND") == 0) return HID_MOD_LGUI;
    return 0;
}

static bool badusb_execute_combo(char *line)
{
    char *cursor = line;
    char *tok;
    uint8_t modifier = 0;
    uint8_t key_mod = 0;
    uint8_t keycode = 0;

    while ((tok = badusb_next_token(&cursor)) != NULL)
    {
        uint8_t mod = badusb_modifier_from_token(tok);
        if (mod != 0)
        {
            modifier |= mod;
            continue;
        }
        if (!badusb_named_key(tok, &key_mod, &keycode))
            return false;
        modifier |= key_mod;
        break;
    }

    if (keycode == 0 && modifier != 0)
        return true;

    return badusb_send_key(modifier, keycode);
}

static bool badusb_execute_line(char *line, uint32_t *line_no)
{
    char *arg;
    uint32_t delay;

    (void)line_no;
    badusb_trim(line);
    if (line[0] == '\0') return true;
    if (badusb_starts_with_word(line, "REM")) return true;
    if (line[0] == '#') return true;

    if (badusb_starts_with_word(line, "DELAY"))
    {
        arg = line + 5;
        badusb_trim(arg);
        delay = (uint32_t)strtoul(arg, NULL, 10);
        return badusb_delay_cancelable(delay);
    }

    if (badusb_starts_with_word(line, "DEFAULT_DELAY") || badusb_starts_with_word(line, "DEFAULTDELAY"))
    {
        arg = strchr(line, ' ');
        if (!arg) arg = strchr(line, '\t');
        if (arg)
        {
            badusb_trim(arg);
            badusb_default_delay_ms = (uint32_t)strtoul(arg, NULL, 10);
        }
        return true;
    }

    if (badusb_starts_with_word(line, "STRINGLN"))
    {
        arg = line + 8;
        badusb_trim(arg);
        return badusb_send_text(arg) && badusb_send_key(0, 0x28);
    }

    if (badusb_starts_with_word(line, "STRING"))
    {
        arg = line + 6;
        badusb_trim(arg);
        return badusb_send_text(arg);
    }

    return badusb_execute_combo(line);
}

static bool badusb_delay_cancelable(uint32_t delay_ms)
{
    uint32_t elapsed = 0;
    uint32_t step;

    while (elapsed < delay_ms)
    {
        if (badusb_user_cancelled()) return false;
        step = delay_ms - elapsed;
        if (step > 25U) step = 25U;
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    return true;
}

static bool badusb_user_cancelled(void)
{
    S_M1_Main_Q_t q_item;
    S_M1_Buttons_Status button_status;
    BaseType_t ret;

    ret = xQueueReceive(main_q_hdl, &q_item, 0);
    if (ret != pdTRUE || q_item.q_evt_type != Q_EVENT_KEYPAD)
        return false;

    ret = xQueueReceive(button_events_q_hdl, &button_status, 0);
    if (ret != pdTRUE)
        return false;

    return button_status.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK;
}

static bool badusb_has_script_extension(const char *name)
{
    const char *ext;

    if (!name) return false;
    ext = strrchr(name, '.');
    if (!ext) return false;
    return strcasecmp(ext, ".txt") == 0 ||
           strcasecmp(ext, ".ducky") == 0 ||
           strcasecmp(ext, ".duck") == 0;
}

static bool badusb_confirm_run(const char *title, const char *name)
{
    S_M1_Main_Q_t q_item;
    S_M1_Buttons_Status button_status;
    BaseType_t ret;

    badusb_confirm_draw(title, name);

    while (1)
    {
        ret = xQueueReceive(main_q_hdl, &q_item, portMAX_DELAY);
        if (ret != pdTRUE || q_item.q_evt_type != Q_EVENT_KEYPAD) continue;
        ret = xQueueReceive(button_events_q_hdl, &button_status, 0);
        if (ret != pdTRUE) continue;

        if (button_status.event[BUTTON_OK_KP_ID] == BUTTON_EVENT_CLICK)
        {
            xQueueReset(main_q_hdl);
            return true;
        }
        if (button_status.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK)
        {
            xQueueReset(main_q_hdl);
            return false;
        }
    }
}

static void badusb_status_draw(const char *title, const char *line1, const char *line2, const char *line3)
{
    m1_u8g2_firstpage();
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
    u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
    m1_draw_text(&m1_u8g2, 2, 11, 124, title ? title : "", TEXT_ALIGN_LEFT);
    u8g2_DrawFrame(&m1_u8g2, 2, 14, 124, 35);
    u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
    m1_draw_text(&m1_u8g2, 8, 26, 112, line1 ? line1 : "", TEXT_ALIGN_LEFT);
    m1_draw_text(&m1_u8g2, 8, 36, 112, line2 ? line2 : "", TEXT_ALIGN_LEFT);
    m1_draw_text(&m1_u8g2, 8, 46, 112, line3 ? line3 : "", TEXT_ALIGN_LEFT);
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
    u8g2_DrawBox(&m1_u8g2, 0, 52, 128, 12);
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
    u8g2_DrawXBMP(&m1_u8g2, 1, 54, 8, 8, arrowleft_8x8);
    m1_draw_text(&m1_u8g2, 11, 61, 50, "BACK", TEXT_ALIGN_LEFT);
    m1_u8g2_nextpage();
}

static void badusb_confirm_draw(const char *title, const char *name)
{
    m1_u8g2_firstpage();
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
    u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
    m1_draw_text(&m1_u8g2, 2, 11, 124, title ? title : "", TEXT_ALIGN_LEFT);
    u8g2_DrawFrame(&m1_u8g2, 2, 14, 124, 35);
    u8g2_SetFont(&m1_u8g2, M1_DISP_SUB_MENU_FONT_N);
    m1_draw_text(&m1_u8g2, 8, 26, 112, "Press OK to run", TEXT_ALIGN_LEFT);
    m1_draw_text(&m1_u8g2, 8, 36, 112, name ? name : "", TEXT_ALIGN_LEFT);
    m1_draw_text(&m1_u8g2, 8, 46, 112, "BACK cancels", TEXT_ALIGN_LEFT);

    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
    u8g2_DrawBox(&m1_u8g2, 0, 52, 128, 12);
    u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_BG);
    u8g2_DrawXBMP(&m1_u8g2, 1, 54, 8, 8, arrowleft_8x8);
    m1_draw_text(&m1_u8g2, 11, 61, 50, "BACK", TEXT_ALIGN_LEFT);
    m1_draw_text(&m1_u8g2, 67, 61, 58, "OK=Run", TEXT_ALIGN_RIGHT);
    m1_u8g2_nextpage();
}

static bool badusb_run_file(const char *path, badusb_transport_t transport)
{
    FIL fil;
    char line[BADUSB_LINE_MAX];
    char status1[32];
    char status2[32];
    uint32_t line_no = 0;
    bool ok = true;

    badusb_transport = transport;

    if (transport == BADUSB_TRANSPORT_USB && !badusb_usb_ready())
    {
        badusb_status_draw("BadUSB", "USB HID not ready", "Connect to host", "Try again");
        badusb_delay_cancelable(1600);
        return false;
    }
    if (transport == BADUSB_TRANSPORT_BLE && !badusb_ble_ready())
    {
        badusb_status_draw("BadBLE", "BLE HID not ready", "Pair/connect first", "Try again");
        badusb_delay_cancelable(1600);
        return false;
    }

    if (f_open(&fil, path, FA_READ) != FR_OK)
    {
        badusb_status_draw(transport == BADUSB_TRANSPORT_BLE ? "BadBLE" : "BadUSB",
                           "Open failed", path, "BACK");
        badusb_delay_cancelable(1200);
        return false;
    }

    badusb_default_delay_ms = BADUSB_DEFAULT_DELAY_MS;
    while (f_gets(line, sizeof(line), &fil) != NULL)
    {
        line_no++;
        if ((line_no % 5U) == 1U)
        {
            snprintf(status1, sizeof(status1), "Running line %lu", (unsigned long)line_no);
            snprintf(status2, sizeof(status2), "%.24s", line);
            badusb_trim(status2);
            badusb_status_draw(transport == BADUSB_TRANSPORT_BLE ? "BadBLE" : "BadUSB",
                               status1, status2, "BACK stops");
        }

        if (!badusb_execute_line(line, &line_no))
        {
            ok = false;
            break;
        }
        if (badusb_default_delay_ms > 0 && !badusb_delay_cancelable(badusb_default_delay_ms))
        {
            ok = false;
            break;
        }
    }

    f_close(&fil);

    if (ok)
        badusb_status_draw(transport == BADUSB_TRANSPORT_BLE ? "BadBLE" : "BadUSB",
                           "Script complete", path, "BACK");
    else
        badusb_status_draw(transport == BADUSB_TRANSPORT_BLE ? "BadBLE" : "BadUSB",
                           "Script stopped", path, "BACK");
    badusb_delay_cancelable(1200);
    xQueueReset(main_q_hdl);
    return ok;
}

static bool badusb_usb_ready(void)
{
    return hUsbDeviceFS.dev_state == USBD_STATE_CONFIGURED;
}

static void badusb_ble_ensure_esp32_ready(void)
{
    if (m1_esp32_get_init_status())
        return;

    m1_esp32_init();
    badusb_status_draw("BadBLE", "Starting ESP32...", NULL, NULL);

    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_RESET);
    HAL_Delay(200);
    HAL_GPIO_WritePin(ESP32_EN_GPIO_Port, ESP32_EN_Pin, GPIO_PIN_SET);
    HAL_Delay(2000);
}

static void badusb_ble_make_session_name(char *out, size_t out_size)
{
    uint8_t suffix;

    if (!out || out_size == 0)
        return;

    suffix = ++badusb_ble_session_id;
    if (suffix == 0)
        suffix = ++badusb_ble_session_id;

    snprintf(out, out_size, "Keyboard-%02X", suffix);
}

static bool badusb_ble_start(const char *name)
{
    m1_cmd_t cmd;
    m1_resp_t resp;
    int rc;
    uint8_t len;

    if (!name || name[0] == '\0')
        name = "Keyboard";

    len = (uint8_t)strlen(name);
    if (len > 29U) len = 29U;

    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = M1_CMD_MAGIC;
    cmd.cmd_id = CMD_BLE_HID_START;
    cmd.payload_len = len;
    memcpy(cmd.payload, name, len);

    rc = m1_esp32_send_cmd(&cmd, &resp, 3000);
    badusb_ble_last_rc = rc;
    badusb_ble_last_status = resp.status;
    return rc == 0 && resp.status == RESP_OK;
}

static bool badusb_ble_stop(void)
{
    m1_resp_t resp;
    int rc = m1_esp32_simple_cmd(CMD_BLE_HID_STOP, &resp, 2000);
    badusb_ble_last_rc = rc;
    badusb_ble_last_status = resp.status;
    return rc == 0 && resp.status == RESP_OK;
}

static bool badusb_ble_status(bool *active, bool *connected, bool *notify_enabled, bool *advertising)
{
    m1_resp_t resp;

    int rc = m1_esp32_simple_cmd(CMD_BLE_HID_STATUS, &resp, 1000);
    badusb_ble_last_rc = rc;
    badusb_ble_last_status = resp.status;

    if (rc != 0 || resp.status != RESP_OK || resp.payload_len < 4)
    {
        return false;
    }

    if (active) *active = resp.payload[0] != 0;
    if (connected) *connected = resp.payload[1] != 0;
    if (notify_enabled) *notify_enabled = resp.payload[2] != 0;
    if (advertising) *advertising = resp.payload[3] != 0;
    return true;
}

static bool badusb_ble_ready(void)
{
    bool connected = false;
    return badusb_ble_status(NULL, &connected, NULL, NULL) && connected;
}

static bool badusb_ble_send_report(uint8_t *report, uint16_t len)
{
    m1_cmd_t cmd;
    m1_resp_t resp;

    if (len != BADUSB_REPORT_LEN)
        return false;

    memset(&cmd, 0, sizeof(cmd));
    cmd.magic = M1_CMD_MAGIC;
    cmd.cmd_id = CMD_BLE_HID_REPORT;
    cmd.payload_len = BADUSB_REPORT_LEN;
    memcpy(cmd.payload, report, BADUSB_REPORT_LEN);

    int rc = m1_esp32_send_cmd(&cmd, &resp, 1000);
    badusb_ble_last_rc = rc;
    badusb_ble_last_status = resp.status;
    return rc == 0 && resp.status == RESP_OK;
}

static bool badusb_ble_wait_connected(const char *name)
{
    S_M1_Main_Q_t q_item;
    S_M1_Buttons_Status button_status;
    BaseType_t ret;
    uint32_t last_draw = 0;
    bool active = false;
    bool connected = false;
    bool notify_enabled = false;
    bool advertising = false;
    char line2[32];

    while (1)
    {
        badusb_ble_status(&active, &connected, &notify_enabled, &advertising);
        if (connected)
            return true;

        if ((HAL_GetTick() - last_draw) > 500U)
        {
            last_draw = HAL_GetTick();
            snprintf(line2, sizeof(line2), "%s", advertising ? "Advertising..." : "Starting...");
            badusb_status_draw("BadBLE", name ? name : "Pair/connect target", line2, "BACK cancels");
        }

        ret = xQueueReceive(main_q_hdl, &q_item, pdMS_TO_TICKS(100));
        if (ret == pdTRUE && q_item.q_evt_type == Q_EVENT_KEYPAD)
        {
            ret = xQueueReceive(button_events_q_hdl, &button_status, 0);
            if (ret == pdTRUE && button_status.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK)
                return false;
        }
    }
}

void badusb_run_script(void)
{
    S_M1_file_info *f_info;
    char path[ESP_FILE_PATH_LEN_MAX + ESP_FILE_NAME_LEN_MAX + 4];

    fs_directory_ensure(BADUSB_SCRIPT_DIR);
    f_info = storage_browse();
    if (!f_info || !f_info->file_is_selected)
        return;

    if (!badusb_has_script_extension(f_info->file_name))
    {
        m1_message_box(&m1_u8g2, "BadUSB", "Use .txt/.ducky", ".duck supported", "BACK");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }

    snprintf(path, sizeof(path), "%s/%s", f_info->dir_name, f_info->file_name);
    if (!badusb_confirm_run("BadUSB", f_info->file_name))
        return;

    badusb_run_file(path, BADUSB_TRANSPORT_USB);
}

void badusb_info(void)
{
    badusb_status_draw("BadUSB", "Scripts: .txt .ducky", "Commands: STRING DELAY", "CTRL ALT GUI SHIFT");
    badusb_delay_cancelable(1800);
    xQueueReset(main_q_hdl);
}

void badble_run_script(void)
{
    S_M1_file_info *f_info;
    char path[ESP_FILE_PATH_LEN_MAX + ESP_FILE_NAME_LEN_MAX + 4];
    char session_name[30];
    char err_line[32];

    fs_directory_ensure(BADUSB_SCRIPT_DIR);
    f_info = storage_browse();
    if (!f_info || !f_info->file_is_selected)
        return;

    if (!badusb_has_script_extension(f_info->file_name))
    {
        m1_message_box(&m1_u8g2, "BadBLE", "Use .txt/.ducky", ".duck supported", "BACK");
        vTaskDelay(pdMS_TO_TICKS(1200));
        return;
    }

    snprintf(path, sizeof(path), "%s/%s", f_info->dir_name, f_info->file_name);
    if (!badusb_confirm_run("BadBLE", f_info->file_name))
        return;

    badusb_ble_ensure_esp32_ready();

    if (!badusb_ble_status(NULL, NULL, NULL, NULL))
    {
        if (badusb_ble_last_rc != 0)
        {
            snprintf(err_line, sizeof(err_line), "SPI rc:%d", badusb_ble_last_rc);
            badusb_status_draw("BadBLE", "ESP32 no response", err_line, "BACK");
        }
        else
        {
            snprintf(err_line, sizeof(err_line), "Status:%u", badusb_ble_last_status);
            badusb_status_draw("BadBLE", "BLE HID missing", err_line, "Flash ESP32 FW");
        }
        badusb_delay_cancelable(2200);
        return;
    }

    badusb_ble_make_session_name(session_name, sizeof(session_name));
    if (!badusb_ble_start(session_name))
    {
        if (badusb_ble_last_rc != 0)
        {
            snprintf(err_line, sizeof(err_line), "SPI rc:%d", badusb_ble_last_rc);
            badusb_status_draw("BadBLE", "Start failed", err_line, "BACK");
        }
        else
        {
            snprintf(err_line, sizeof(err_line), "ESP status:%u", badusb_ble_last_status);
            badusb_status_draw("BadBLE", "Start failed", err_line, "Flash ESP32 FW");
        }
        badusb_delay_cancelable(2200);
        return;
    }

    if (badusb_ble_wait_connected(session_name))
    {
        badusb_run_file(path, BADUSB_TRANSPORT_BLE);
    }

    badusb_ble_stop();
    xQueueReset(main_q_hdl);
}
