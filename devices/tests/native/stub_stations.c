/* The stations half, absent: the equipment tests link the real shell and
 * the real things.c against this. */
#include "../../../hal/xprs_wasm_hal.h"
#include "../../devices.h"
void st_init(void) {}
void st_event(void) {}
void st_command(void) {}
int  st_screen(void) { return 0; }
const char *st_sections(int part) { (void)part; return ""; }
