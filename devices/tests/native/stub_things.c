/* The equipment half, absent: the stations tests link the real shell and
 * the real stations.c against this. */
#include "../../../hal/xprs_wasm_hal.h"
#include "../../devices.h"
void th_init(void) {}
void th_event(void) {}
void th_pump(void) {}
void th_command(void) {}
int  th_screen(void) { return 0; }
int  th_has(const char *call) { (void)call; return 0; }
void th_open(const char *call) { (void)call; }
void th_list_prepare(void) {}
const char *th_sections(int part) { (void)part; return ""; }
