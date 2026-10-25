/* The native Mac host of the wasm module (finding 075): what its files share. */
#ifndef DHD_MAC_NATIVE_H
#define DHD_MAC_NATIVE_H

#include <stddef.h>
#include <stdint.h>
#include "dhd.h"  /* wasm2c's header of the module (build/port-mac/link/c) */

/* The module's memory: the PS2 address space, 4 GB (space.c). */
extern uint8_t *space;
static inline void *at(uint32_t address) { return space + address; }
static inline const char *string_at(uint32_t address) { return (const char *)(space + address); }

/* Each host thread runs the module on its own copy of the instance: shared memory and function
   table, its own module stack pointer (the only mutable global). */
extern w2c_dhd main_instance;
extern _Thread_local w2c_dhd *thread_instance;
uint32_t *instance_stack_pointer(w2c_dhd *instance);

void space_init(void);
void fpcr_ee_default(void);      /* fenv.c: the mode a new host thread starts in */
void fibers_thread_init(void);   /* fiber.c: the calling thread is fiber 0 of its own */
void app_folder_init(void);      /* files.c */
const char *app_folder(void);    /* with a trailing '/' */
void log_init(void);

void pads_stop(void);  /* pads.m: no pad left vibrating at exit */

#endif
