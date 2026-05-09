/* See COPYING.txt for license details. */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "FreeRTOS.h"
#include "stm32h5xx_hal.h"
#include "main.h"
#include "ff.h"
#include "m1_apps.h"
#include "m1_buzzer.h"
#include "m1_core_config.h"
#include "m1_lcd.h"
#include "m1_lp5814.h"
#include "m1_sdcard.h"
#include "task.h"

#define APPS_DIR                    "apps"
#define APPS_EXT                    ".m1app"
#define APPS_MAX                    32
#define APP_NAME_MAX                48
#define APP_PATH_MAX                96
#define APP_ELF_MAX_SECTIONS        64
#define APP_API_VERSION             2
#define APP_MANIFEST_MAGIC          0x4D314150UL

#define EI_NIDENT                   16
#define ET_REL                      1
#define EM_ARM                      40
#define SHT_PROGBITS                1
#define SHT_SYMTAB                  2
#define SHT_STRTAB                  3
#define SHT_NOBITS                  8
#define SHT_REL                     9
#define SHF_ALLOC                   0x02
#define SHN_UNDEF                   0
#define R_ARM_ABS32                 2

typedef struct
{
	uint8_t  e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint32_t e_entry;
	uint32_t e_phoff;
	uint32_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
} elf32_ehdr_t;

typedef struct
{
	uint32_t sh_name;
	uint32_t sh_type;
	uint32_t sh_flags;
	uint32_t sh_addr;
	uint32_t sh_offset;
	uint32_t sh_size;
	uint32_t sh_link;
	uint32_t sh_info;
	uint32_t sh_addralign;
	uint32_t sh_entsize;
} elf32_shdr_t;

typedef struct
{
	uint32_t st_name;
	uint32_t st_value;
	uint32_t st_size;
	uint8_t  st_info;
	uint8_t  st_other;
	uint16_t st_shndx;
} elf32_sym_t;

typedef struct
{
	uint32_t r_offset;
	uint32_t r_info;
} elf32_rel_t;

typedef struct __attribute__((packed))
{
	uint32_t magic;
	uint16_t api_version;
	uint16_t stack_size;
	char name[32];
} app_manifest_t;

typedef int32_t (*app_entry_t)(void *context);

typedef struct
{
	uint8_t *image;
	uint32_t image_size;
	uint8_t *mem;
	uint32_t mem_size;
	app_entry_t entry;
	app_manifest_t manifest;
	const char *error;
	char missing_symbol[40];
} loaded_app_t;

typedef struct
{
	loaded_app_t *app;
	volatile uint8_t done;
	int32_t result;
} app_task_ctx_t;

typedef enum
{
	M1APP_BTN_NONE = 0,
	M1APP_BTN_UP,
	M1APP_BTN_DOWN,
	M1APP_BTN_LEFT,
	M1APP_BTN_RIGHT,
	M1APP_BTN_OK,
	M1APP_BTN_BACK
} m1app_button_t;

static char app_names[APPS_MAX][APP_NAME_MAX];
static uint8_t app_count;

static void apps_draw_message(const char *title, const char *line1, const char *line2, const char *line3);
static void apps_message(const char *title, const char *line1, const char *line2, const char *line3);

static uint32_t apps_align_up(uint32_t value, uint32_t align)
{
	if (align <= 1)
	{
		return value;
	}

	return (value + align - 1U) & ~(align - 1U);
}

static bool apps_range_ok(uint32_t off, uint32_t size, uint32_t total)
{
	return off <= total && size <= (total - off);
}

static const char *apps_section_name(const loaded_app_t *app, const elf32_shdr_t *shdr, uint16_t idx)
{
	const elf32_ehdr_t *eh = (const elf32_ehdr_t *)app->image;
	const elf32_shdr_t *shstr;

	if (idx >= eh->e_shnum || eh->e_shstrndx >= eh->e_shnum)
	{
		return "";
	}

	shstr = &shdr[eh->e_shstrndx];
	if (!apps_range_ok(shstr->sh_offset, shstr->sh_size, app->image_size))
	{
		return "";
	}
	if (shdr[idx].sh_name >= shstr->sh_size)
	{
		return "";
	}

	return (const char *)(app->image + shstr->sh_offset + shdr[idx].sh_name);
}

static char app_ascii_lower(char c)
{
	if (c >= 'A' && c <= 'Z')
	{
		return (char)(c + ('a' - 'A'));
	}

	return c;
}

static bool app_has_ext(const char *name, const char *ext)
{
	size_t name_len = strlen(name);
	size_t ext_len = strlen(ext);

	if (name_len < ext_len)
	{
		return false;
	}

	name += name_len - ext_len;
	for (size_t i = 0; i < ext_len; i++)
	{
		if (app_ascii_lower(name[i]) != app_ascii_lower(ext[i]))
		{
			return false;
		}
	}

	return true;
}

static uint32_t apps_fn_addr(void (*fn)(void))
{
	union
	{
		void (*fn)(void);
		uint32_t addr;
	} u;

	u.fn = fn;
	return u.addr;
}

static uint32_t apps_obj_addr(const void *obj)
{
	return (uint32_t)(uintptr_t)obj;
}

static u8g2_t *m1app_get_u8g2(void)
{
	return &m1_u8g2;
}

static void m1app_display_begin(void)
{
	m1_u8g2_firstpage();
}

static uint8_t m1app_display_flush(void)
{
	return m1_u8g2_nextpage();
}

static void *m1app_malloc(size_t size)
{
	return m1_malloc(size);
}

static void m1app_free(void *ptr)
{
	m1_free(ptr);
}

static void m1app_delay(uint32_t ms)
{
	vTaskDelay(pdMS_TO_TICKS(ms));
}

static uint32_t m1app_get_tick(void)
{
	return HAL_GetTick();
}

static m1app_button_t game_poll_button(uint32_t timeout_ms)
{
	S_M1_Buttons_Status btn;
	S_M1_Main_Q_t q_item;
	TickType_t timeout = (timeout_ms == 0xFFFFFFFFUL) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);

	if (xQueueReceive(main_q_hdl, &q_item, timeout) != pdTRUE ||
		q_item.q_evt_type != Q_EVENT_KEYPAD ||
		xQueueReceive(button_events_q_hdl, &btn, 0) != pdTRUE)
	{
		return M1APP_BTN_NONE;
	}

	if (btn.event[BUTTON_UP_KP_ID] == BUTTON_EVENT_CLICK) return M1APP_BTN_UP;
	if (btn.event[BUTTON_DOWN_KP_ID] == BUTTON_EVENT_CLICK) return M1APP_BTN_DOWN;
	if (btn.event[BUTTON_LEFT_KP_ID] == BUTTON_EVENT_CLICK) return M1APP_BTN_LEFT;
	if (btn.event[BUTTON_RIGHT_KP_ID] == BUTTON_EVENT_CLICK) return M1APP_BTN_RIGHT;
	if (btn.event[BUTTON_OK_KP_ID] == BUTTON_EVENT_CLICK) return M1APP_BTN_OK;
	if (btn.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK) return M1APP_BTN_BACK;
	return M1APP_BTN_NONE;
}

static void game_rand_seed(void)
{
	srand(HAL_GetTick());
}

static int game_rand_range(int min, int max)
{
	if (max <= min)
	{
		return min;
	}

	return min + (rand() % ((max - min) + 1));
}

static int m1_field_detect_start(void)
{
	return 0;
}

static void m1_field_detect_stop(void)
{
}

static int m1_field_detect_nfc(void)
{
	return 0;
}

static int m1_field_detect_rfid(uint32_t *frequency)
{
	if (frequency != NULL)
	{
		*frequency = 0;
	}
	return 0;
}

static int m1_field_detect_rfid_raw(void)
{
	return -1;
}

static int m1_field_detect_nfc_raw(void)
{
	return -1;
}

static int m1_field_detect_nfc_opctl(void)
{
	return -1;
}

static bool apps_lookup_symbol(const char *name, uint32_t *addr)
{
	typedef struct
	{
		const char *name;
		uint32_t addr;
	} symbol_t;

	const symbol_t symbols[] =
	{
		{"m1app_get_u8g2", apps_fn_addr((void (*)(void))m1app_get_u8g2)},
		{"m1app_display_begin", apps_fn_addr((void (*)(void))m1app_display_begin)},
		{"m1app_display_flush", apps_fn_addr((void (*)(void))m1app_display_flush)},
		{"m1app_delay", apps_fn_addr((void (*)(void))m1app_delay)},
		{"m1app_get_tick", apps_fn_addr((void (*)(void))m1app_get_tick)},
		{"m1app_malloc", apps_fn_addr((void (*)(void))m1app_malloc)},
		{"m1app_free", apps_fn_addr((void (*)(void))m1app_free)},
		{"game_poll_button", apps_fn_addr((void (*)(void))game_poll_button)},
		{"game_rand_seed", apps_fn_addr((void (*)(void))game_rand_seed)},
		{"game_rand_range", apps_fn_addr((void (*)(void))game_rand_range)},
		{"m1_field_detect_start", apps_fn_addr((void (*)(void))m1_field_detect_start)},
		{"m1_field_detect_stop", apps_fn_addr((void (*)(void))m1_field_detect_stop)},
		{"m1_field_detect_nfc", apps_fn_addr((void (*)(void))m1_field_detect_nfc)},
		{"m1_field_detect_rfid", apps_fn_addr((void (*)(void))m1_field_detect_rfid)},
		{"m1_field_detect_rfid_raw", apps_fn_addr((void (*)(void))m1_field_detect_rfid_raw)},
		{"m1_field_detect_nfc_raw", apps_fn_addr((void (*)(void))m1_field_detect_nfc_raw)},
		{"m1_field_detect_nfc_opctl", apps_fn_addr((void (*)(void))m1_field_detect_nfc_opctl)},
		{"m1_lcd_cleardisplay", apps_fn_addr((void (*)(void))m1_lcd_cleardisplay)},
		{"m1_buzzer_notification", apps_fn_addr((void (*)(void))m1_buzzer_notification)},
		{"m1_buzzer_notification2", apps_fn_addr((void (*)(void))m1_buzzer_notification2)},
		{"m1_buzzer_set", apps_fn_addr((void (*)(void))m1_buzzer_set)},
		{"lp5814_led_on_Red", apps_fn_addr((void (*)(void))lp5814_led_on_Red)},
		{"lp5814_led_on_Green", apps_fn_addr((void (*)(void))lp5814_led_on_Green)},
		{"lp5814_led_on_Blue", apps_fn_addr((void (*)(void))lp5814_led_on_Blue)},
		{"lp5814_all_off_RGB", apps_fn_addr((void (*)(void))lp5814_all_off_RGB)},
		{"u8g2_FirstPage", apps_fn_addr((void (*)(void))u8g2_FirstPage)},
		{"u8g2_DrawStr", apps_fn_addr((void (*)(void))u8g2_DrawStr)},
		{"u8g2_DrawBox", apps_fn_addr((void (*)(void))u8g2_DrawBox)},
		{"u8g2_DrawFrame", apps_fn_addr((void (*)(void))u8g2_DrawFrame)},
		{"u8g2_DrawPixel", apps_fn_addr((void (*)(void))u8g2_DrawPixel)},
		{"u8g2_DrawLine", apps_fn_addr((void (*)(void))u8g2_DrawLine)},
		{"u8g2_DrawCircle", apps_fn_addr((void (*)(void))u8g2_DrawCircle)},
		{"u8g2_DrawDisc", apps_fn_addr((void (*)(void))u8g2_DrawDisc)},
		{"u8g2_DrawHLine", apps_fn_addr((void (*)(void))u8g2_DrawHLine)},
		{"u8g2_DrawVLine", apps_fn_addr((void (*)(void))u8g2_DrawVLine)},
		{"u8g2_DrawXBM", apps_fn_addr((void (*)(void))u8g2_DrawXBM)},
		{"u8g2_SetDrawColor", apps_fn_addr((void (*)(void))u8g2_SetDrawColor)},
		{"u8g2_SetFont", apps_fn_addr((void (*)(void))u8g2_SetFont)},
		{"snprintf", apps_fn_addr((void (*)(void))snprintf)},
		{"memset", apps_fn_addr((void (*)(void))memset)},
		{"memcpy", apps_fn_addr((void (*)(void))memcpy)},
		{"memcmp", apps_fn_addr((void (*)(void))memcmp)},
		{"strlen", apps_fn_addr((void (*)(void))strlen)},
		{"strcmp", apps_fn_addr((void (*)(void))strcmp)},
		{"strncmp", apps_fn_addr((void (*)(void))strncmp)},
		{"strncpy", apps_fn_addr((void (*)(void))strncpy)},
		{"strstr", apps_fn_addr((void (*)(void))strstr)},
		{"strchr", apps_fn_addr((void (*)(void))strchr)},
		{"rand", apps_fn_addr((void (*)(void))rand)},
		{"srand", apps_fn_addr((void (*)(void))srand)},
		{"abs", apps_fn_addr((void (*)(void))abs)},
		{"u8g2_font_4x6_tr", apps_obj_addr(u8g2_font_4x6_tr)},
		{"u8g2_font_5x8_tr", apps_obj_addr(u8g2_font_5x8_tr)},
		{"u8g2_font_6x10_tr", apps_obj_addr(u8g2_font_6x10_tr)},
		{"u8g2_font_helvB08_tr", apps_obj_addr(u8g2_font_helvB08_tr)},
		{"u8g2_font_helvB08_tf", apps_obj_addr(u8g2_font_helvB08_tf)},
		{"u8g2_font_spleen5x8_mf", apps_obj_addr(u8g2_font_spleen5x8_mf)},
		{"u8g2_font_spleen8x16_mf", apps_obj_addr(u8g2_font_spleen8x16_mf)},
		{"u8g2_font_Terminal_tr", apps_obj_addr(u8g2_font_Terminal_tr)},
		{"u8g2_font_profont17_tr", apps_obj_addr(u8g2_font_profont17_tr)},
		{"u8g2_font_VCR_OSD_tu", apps_obj_addr(u8g2_font_VCR_OSD_tu)},
		{"u8g2_font_Pixellari_tu", apps_obj_addr(u8g2_font_Pixellari_tu)},
		{"u8g2_font_pcsenior_8f", apps_obj_addr(u8g2_font_pcsenior_8f)}
	};

	for (uint32_t i = 0; i < (sizeof(symbols) / sizeof(symbols[0])); i++)
	{
		if (strcmp(symbols[i].name, name) == 0)
		{
			*addr = symbols[i].addr;
			return true;
		}
	}

	return false;
}

static bool apps_resolve_sym(const loaded_app_t *app, const elf32_shdr_t *shdr,
							 const elf32_sym_t *symtab, const char *strtab,
							 uint8_t *section_addr[], uint32_t sym_idx,
							 uint32_t *value)
{
	const elf32_sym_t *sym = &symtab[sym_idx];

	if (sym->st_shndx == SHN_UNDEF)
	{
		const char *name = strtab + sym->st_name;
		if (!apps_lookup_symbol(name, value))
		{
			strncpy(((loaded_app_t *)app)->missing_symbol, name, sizeof(app->missing_symbol) - 1);
			((loaded_app_t *)app)->missing_symbol[sizeof(app->missing_symbol) - 1] = '\0';
			((loaded_app_t *)app)->error = "Missing symbol";
			return false;
		}
		return true;
	}

	if (sym->st_shndx >= APP_ELF_MAX_SECTIONS || section_addr[sym->st_shndx] == NULL)
	{
		((loaded_app_t *)app)->error = "Bad symbol";
		return false;
	}

	(void)shdr;
	*value = (uint32_t)(uintptr_t)(section_addr[sym->st_shndx] + sym->st_value);
	return true;
}

static bool apps_find_entry(const loaded_app_t *app, const elf32_sym_t *symtab,
							uint32_t sym_count, const char *strtab,
							uint8_t *section_addr[], uint32_t *entry)
{
	for (uint32_t i = 0; i < sym_count; i++)
	{
		if (symtab[i].st_name == 0)
		{
			continue;
		}
		if (strcmp(strtab + symtab[i].st_name, "app_main") == 0)
		{
			if (symtab[i].st_shndx == SHN_UNDEF ||
				symtab[i].st_shndx >= APP_ELF_MAX_SECTIONS ||
				section_addr[symtab[i].st_shndx] == NULL)
			{
				((loaded_app_t *)app)->error = "Bad app_main";
				return false;
			}

			*entry = (uint32_t)(uintptr_t)(section_addr[symtab[i].st_shndx] + symtab[i].st_value);
			return true;
		}
	}

	((loaded_app_t *)app)->error = "No app_main";
	return false;
}

static bool apps_apply_relocations(loaded_app_t *app, const elf32_shdr_t *shdr,
								   const elf32_sym_t *symtab, uint32_t sym_count,
								   const char *strtab, uint8_t *section_addr[],
								   uint32_t section_size[])
{
	const elf32_ehdr_t *eh = (const elf32_ehdr_t *)app->image;

	for (uint16_t i = 0; i < eh->e_shnum; i++)
	{
		if (shdr[i].sh_type != SHT_REL)
		{
			continue;
		}
		if (shdr[i].sh_info >= eh->e_shnum ||
			section_addr[shdr[i].sh_info] == NULL ||
			shdr[i].sh_entsize != sizeof(elf32_rel_t) ||
			!apps_range_ok(shdr[i].sh_offset, shdr[i].sh_size, app->image_size))
		{
			app->error = "Bad relocation";
			return false;
		}

		const elf32_rel_t *rel = (const elf32_rel_t *)(app->image + shdr[i].sh_offset);
		uint32_t rel_count = shdr[i].sh_size / sizeof(elf32_rel_t);
		for (uint32_t r = 0; r < rel_count; r++)
		{
			uint32_t type = rel[r].r_info & 0xFFU;
			uint32_t sym_idx = rel[r].r_info >> 8;
			uint32_t sym_value = 0;
			uint8_t *target = section_addr[shdr[i].sh_info];

			if (type != R_ARM_ABS32 || sym_idx >= sym_count ||
				rel[r].r_offset + sizeof(uint32_t) > section_size[shdr[i].sh_info])
			{
				app->error = "Reloc unsupported";
				return false;
			}
			if (!apps_resolve_sym(app, shdr, symtab, strtab, section_addr, sym_idx, &sym_value))
			{
				return false;
			}

			uint32_t *place = (uint32_t *)(target + rel[r].r_offset);
			*place = *place + sym_value;
		}
	}

	return true;
}

static bool apps_load_elf(const char *name, loaded_app_t *app)
{
	FIL fil;
	UINT br = 0;
	char path[APP_PATH_MAX];
	uint32_t cursor = 0;
	uint8_t *section_addr[APP_ELF_MAX_SECTIONS] = {0};
	uint32_t section_size[APP_ELF_MAX_SECTIONS] = {0};
	const elf32_shdr_t *shdr;
	const elf32_sym_t *symtab = NULL;
	const char *strtab = NULL;
	uint32_t sym_count = 0;
	uint32_t entry = 0;
	bool found_manifest = false;

	memset(app, 0, sizeof(*app));
	snprintf(path, sizeof(path), "%s/%s", APPS_DIR, name);
	if (f_open(&fil, path, FA_READ) != FR_OK)
	{
		app->error = "Open failed";
		return false;
	}

	app->image_size = (uint32_t)f_size(&fil);
	if (app->image_size < sizeof(elf32_ehdr_t) || app->image_size > (128UL * 1024UL))
	{
		f_close(&fil);
		app->error = "Bad file size";
		return false;
	}

	app->image = m1_malloc(app->image_size);
	if (app->image == NULL)
	{
		f_close(&fil);
		app->error = "No memory";
		return false;
	}

	if (f_read(&fil, app->image, app->image_size, &br) != FR_OK || br != app->image_size)
	{
		f_close(&fil);
		app->error = "Read failed";
		return false;
	}
	f_close(&fil);

	const elf32_ehdr_t *eh = (const elf32_ehdr_t *)app->image;
	if (eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
		eh->e_ident[3] != 'F' || eh->e_ident[4] != 1 || eh->e_ident[5] != 1 ||
		eh->e_type != ET_REL || eh->e_machine != EM_ARM ||
		eh->e_shnum == 0 || eh->e_shnum > APP_ELF_MAX_SECTIONS ||
		eh->e_shentsize != sizeof(elf32_shdr_t) ||
		!apps_range_ok(eh->e_shoff, (uint32_t)eh->e_shentsize * eh->e_shnum, app->image_size))
	{
		app->error = "Bad ELF";
		return false;
	}

	shdr = (const elf32_shdr_t *)(app->image + eh->e_shoff);
	for (uint16_t i = 0; i < eh->e_shnum; i++)
	{
		if ((shdr[i].sh_flags & SHF_ALLOC) == 0)
		{
			continue;
		}
		cursor = apps_align_up(cursor, shdr[i].sh_addralign);
		section_size[i] = shdr[i].sh_size;
		cursor += shdr[i].sh_size;
	}

	app->mem_size = apps_align_up(cursor, 8);
	if (app->mem_size == 0 || app->mem_size > (96UL * 1024UL))
	{
		app->error = "App too large";
		return false;
	}

	app->mem = m1_malloc(app->mem_size);
	if (app->mem == NULL)
	{
		app->error = "No app RAM";
		return false;
	}
	memset(app->mem, 0, app->mem_size);

	cursor = 0;
	for (uint16_t i = 0; i < eh->e_shnum; i++)
	{
		const char *sec_name;
		if ((shdr[i].sh_flags & SHF_ALLOC) == 0)
		{
			continue;
		}

		cursor = apps_align_up(cursor, shdr[i].sh_addralign);
		section_addr[i] = app->mem + cursor;
		if (shdr[i].sh_type == SHT_PROGBITS)
		{
			if (!apps_range_ok(shdr[i].sh_offset, shdr[i].sh_size, app->image_size))
			{
				app->error = "Bad section";
				return false;
			}
			memcpy(section_addr[i], app->image + shdr[i].sh_offset, shdr[i].sh_size);
		}
		else if (shdr[i].sh_type != SHT_NOBITS)
		{
			app->error = "Bad section type";
			return false;
		}

		sec_name = apps_section_name(app, shdr, i);
		if (strcmp(sec_name, ".m1meta") == 0 && shdr[i].sh_size >= sizeof(app_manifest_t))
		{
			memcpy(&app->manifest, section_addr[i], sizeof(app_manifest_t));
			found_manifest = true;
		}
		cursor += shdr[i].sh_size;
	}

	if (!found_manifest || app->manifest.magic != APP_MANIFEST_MAGIC ||
		app->manifest.api_version > APP_API_VERSION || app->manifest.stack_size < 256)
	{
		app->error = "Bad manifest";
		return false;
	}
	app->manifest.name[sizeof(app->manifest.name) - 1] = '\0';

	for (uint16_t i = 0; i < eh->e_shnum; i++)
	{
		if (shdr[i].sh_type == SHT_SYMTAB)
		{
			if (shdr[i].sh_entsize != sizeof(elf32_sym_t) ||
				shdr[i].sh_link >= eh->e_shnum ||
				!apps_range_ok(shdr[i].sh_offset, shdr[i].sh_size, app->image_size) ||
				!apps_range_ok(shdr[shdr[i].sh_link].sh_offset, shdr[shdr[i].sh_link].sh_size, app->image_size))
			{
				app->error = "Bad symtab";
				return false;
			}
			symtab = (const elf32_sym_t *)(app->image + shdr[i].sh_offset);
			strtab = (const char *)(app->image + shdr[shdr[i].sh_link].sh_offset);
			sym_count = shdr[i].sh_size / sizeof(elf32_sym_t);
			break;
		}
	}

	if (symtab == NULL || strtab == NULL)
	{
		app->error = "No symbols";
		return false;
	}
	if (!apps_apply_relocations(app, shdr, symtab, sym_count, strtab, section_addr, section_size) ||
		!apps_find_entry(app, symtab, sym_count, strtab, section_addr, &entry))
	{
		return false;
	}

	HAL_ICACHE_Invalidate();
	app->entry = (app_entry_t)(uintptr_t)entry;
	return true;
}

static void apps_unload(loaded_app_t *app)
{
	if (app->mem != NULL)
	{
		m1_free(app->mem);
	}
	if (app->image != NULL)
	{
		m1_free(app->image);
	}
	memset(app, 0, sizeof(*app));
}

static void apps_task(void *param)
{
	app_task_ctx_t *ctx = (app_task_ctx_t *)param;
	ctx->result = ctx->app->entry(NULL);
	ctx->done = 1;
	vTaskDelete(NULL);
}

static void apps_run_loaded(loaded_app_t *app)
{
	app_task_ctx_t ctx;
	TaskHandle_t task = NULL;
	BaseType_t ret;
	char line[24];

	memset(&ctx, 0, sizeof(ctx));
	ctx.app = app;
	xQueueReset(main_q_hdl);
	ret = xTaskCreate(apps_task, app->manifest.name, app->manifest.stack_size,
					  &ctx, TASK_PRIORITY_SUBFUNC_HANDLER, &task);
	if (ret != pdPASS || task == NULL)
	{
		apps_message("Apps", "Launch failed", "No task memory", "");
		return;
	}

	while (!ctx.done)
	{
		vTaskDelay(pdMS_TO_TICKS(50));
	}

	snprintf(line, sizeof(line), "Exit %ld", (long)ctx.result);
	apps_message("Apps", app->manifest.name, line, "");
}

static void apps_wait_back(void)
{
	S_M1_Buttons_Status btn;
	S_M1_Main_Q_t q_item;

	while (1)
	{
		if (xQueueReceive(main_q_hdl, &q_item, portMAX_DELAY) == pdTRUE &&
			q_item.q_evt_type == Q_EVENT_KEYPAD &&
			xQueueReceive(button_events_q_hdl, &btn, 0) == pdTRUE)
		{
			if (btn.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK ||
				btn.event[BUTTON_OK_KP_ID] == BUTTON_EVENT_CLICK)
			{
				xQueueReset(main_q_hdl);
				return;
			}
		}
	}
}

static void apps_message(const char *title, const char *line1, const char *line2, const char *line3)
{
	apps_draw_message(title, line1, line2, line3);
	apps_wait_back();
}

static void apps_draw_message(const char *title, const char *line1, const char *line2, const char *line3)
{
	m1_u8g2_firstpage();
	do
	{
		u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
		u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
		u8g2_DrawStr(&m1_u8g2, 2, 11, title);
		u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);
		if (line1 != NULL) u8g2_DrawStr(&m1_u8g2, 2, 25, line1);
		if (line2 != NULL) u8g2_DrawStr(&m1_u8g2, 2, 36, line2);
		if (line3 != NULL) u8g2_DrawStr(&m1_u8g2, 2, 47, line3);
		u8g2_DrawStr(&m1_u8g2, 2, 63, "[OK/BACK]");
	} while (m1_u8g2_nextpage());
}

static uint8_t apps_load_list(void)
{
	DIR dir;
	FILINFO info;

	app_count = 0;
	f_mkdir(APPS_DIR);

	if (f_opendir(&dir, APPS_DIR) != FR_OK)
	{
		return 0;
	}

	while (app_count < APPS_MAX && f_readdir(&dir, &info) == FR_OK && info.fname[0] != '\0')
	{
		if ((info.fattrib & AM_DIR) != 0)
		{
			continue;
		}
		if (!app_has_ext(info.fname, APPS_EXT))
		{
			continue;
		}

		strncpy(app_names[app_count], info.fname, APP_NAME_MAX - 1);
		app_names[app_count][APP_NAME_MAX - 1] = '\0';
		app_count++;
	}

	f_closedir(&dir);
	return app_count;
}

static void apps_run_index(uint8_t idx)
{
	loaded_app_t app;
	char line2[24] = {0};

	apps_draw_message("Apps", "Loading...", app_names[idx], "");
	if (apps_load_elf(app_names[idx], &app))
	{
		apps_run_loaded(&app);
		apps_unload(&app);
		return;
	}

	if (app.missing_symbol[0] != '\0')
	{
		snprintf(line2, sizeof(line2), "%.23s", app.missing_symbol);
		apps_message("Load Error", app.error, line2, "");
	}
	else
	{
		apps_message("Load Error", app.error ? app.error : "Unknown", app_names[idx], "");
	}
	apps_unload(&app);
}

static void apps_draw_list(uint8_t sel, uint8_t top)
{
	char title[24];

	m1_u8g2_firstpage();
	do
	{
		u8g2_SetDrawColor(&m1_u8g2, M1_DISP_DRAW_COLOR_TXT);
		u8g2_SetFont(&m1_u8g2, M1_DISP_MAIN_MENU_FONT_N);
		snprintf(title, sizeof(title), "Apps %u", (unsigned)app_count);
		u8g2_DrawStr(&m1_u8g2, 2, 11, title);
		u8g2_SetFont(&m1_u8g2, M1_DISP_FUNC_MENU_FONT_N);

		for (uint8_t row = 0; row < 4 && (top + row) < app_count; row++)
		{
			uint8_t i = top + row;
			uint8_t y = 23 + row * 10;
			char line[24];
			snprintf(line, sizeof(line), "%c %.21s", (i == sel) ? '>' : ' ', app_names[i]);
			u8g2_DrawStr(&m1_u8g2, 2, y, line);
		}

		u8g2_DrawStr(&m1_u8g2, 2, 63, "[OK] Run [BACK]");
	} while (m1_u8g2_nextpage());
}

void external_apps_browse(void)
{
	S_M1_Buttons_Status btn;
	S_M1_Main_Q_t q_item;
	uint8_t sel = 0;
	uint8_t top = 0;

	if (m1_sdcard_get_status() != SD_access_OK)
	{
		m1_sdcard_init_retry();
	}

	if (apps_load_list() == 0)
	{
		apps_message("Apps", "No .m1app files", "Put apps in", "SD:/apps/");
		return;
	}

	apps_draw_list(sel, top);
	while (1)
	{
		if (xQueueReceive(main_q_hdl, &q_item, portMAX_DELAY) != pdTRUE ||
			q_item.q_evt_type != Q_EVENT_KEYPAD ||
			xQueueReceive(button_events_q_hdl, &btn, 0) != pdTRUE)
		{
			continue;
		}

		if (btn.event[BUTTON_BACK_KP_ID] == BUTTON_EVENT_CLICK)
		{
			xQueueReset(main_q_hdl);
			return;
		}
		else if (btn.event[BUTTON_UP_KP_ID] == BUTTON_EVENT_CLICK)
		{
			sel = (sel == 0) ? (app_count - 1) : (sel - 1);
		}
		else if (btn.event[BUTTON_DOWN_KP_ID] == BUTTON_EVENT_CLICK)
		{
			sel = (sel + 1) % app_count;
		}
		else if (btn.event[BUTTON_OK_KP_ID] == BUTTON_EVENT_CLICK)
		{
			apps_run_index(sel);
		}

		if (sel < top) top = sel;
		if (sel >= top + 4) top = sel - 3;
		apps_draw_list(sel, top);
	}
}

void external_apps_sdk_info(void)
{
	apps_message("M1-SDK", "Apps: SD:/apps", "Ext: .m1app", "SDK: M1-SDK");
}
