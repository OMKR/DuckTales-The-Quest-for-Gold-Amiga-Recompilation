/* High-level emulation of the AmigaOS libraries and custom chips. */
#ifndef HLE_H
#define HLE_H

#include <stdint.h>

/* Memory map (24-bit address space):
 *   0x000000-0x0003FF  vectors, ExecBase pointer at 4
 *   0x010000-LOAD_END  program hunks (see gen/layout.h)
 *   0x060000-0x07FFFF  initial stack
 *   0x080000-0xBEFFFF  AllocMem heap (all of it behaves as chip memory)
 *   0xF00000-0xFEFFFF  fake library bases and OS structures
 */
#define HLE_STACK_TOP  0x00080000u
#define HLE_HEAP_START 0x00080000u
#define HLE_HEAP_END   0x00BF0000u
#define HLE_LIB_AREA   0x00F00000u
#define HLE_LIB_STRIDE 0x00010000u
#define HLE_LIB_COUNT  15
#define HLE_EXIT_ADDR  0x00FFFFF0u   /* return address of the program entry */
#define HLE_CALLBACK_ADDR 0x00FFFFF4u /* return address for OS callbacks */

typedef enum {
    LIB_EXEC, LIB_DOS, LIB_GRAPHICS, LIB_INTUITION, LIB_LAYERS, LIB_ICON,
    LIB_MATHFFP, LIB_DEV_INPUT, LIB_DEV_AUDIO, LIB_DEV_TIMER, LIB_DEV_TRACKDISK,
    LIB_DEV_CONSOLE, LIB_UNKNOWN
} LibId;

void hle_init(const char *gamedir, uint32_t load_end);
uint32_t hle_alloc_string(const char *s);
uint32_t hle_alloc(uint32_t size, int clear);
void hle_free(uint32_t addr, uint32_t size);
int hle_is_library_vector(uint32_t addr);
void hle_call_vector(uint32_t addr);
uint32_t hle_lib_base(LibId id);
const char *hle_lvo_name(LibId id, int offset);

/* Per-library handlers; return 0 if the vector is not implemented. */
int hle_exec(int offset);
int hle_dos(int offset);
int hle_graphics(int offset);
int hle_intuition(int offset);
int hle_layers(int offset);
int hle_icon(int offset);
int hle_mathffp(int offset);
int hle_device(LibId id, int offset);

int hle_device_open(const char *name, uint32_t ioreq);
void hle_device_io(uint32_t ioreq);

/* Call recompiled code at `code` with a0/a1 set, preserving all registers
 * except d0, which is returned. hle_callback_a3 also keeps a3 updated. */
uint32_t hle_callback(uint32_t code, uint32_t a0, uint32_t a1);
uint32_t hle_callback_a3(uint32_t code, uint32_t a3);

extern int hle_trace;
extern const char *hle_gamedir;

#endif
