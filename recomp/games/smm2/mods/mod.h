// SMM2 v3.0.3 mods: shared declarations (games/smm2/mods/*.c). Addresses are v3.0.3; addresses are offsets in main
// (MAIN_ADDR). Course World itself is OpenCourse World's (ocw.c): the game's own online client runs unmodified.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../../../runtime/hle.h"

#define MAIN_ADDR(off) (MAIN_BASE + (off))  // an offset in main -> its address (runtime/rt.h)

// ---- settings (settings.c): environment variables, else smm2mods.ini in the data folder
typedef struct {
    bool log;            // RECOMP_MOD_LOG / log=1: log the mods' overrides
} ModSettings;
extern ModSettings g_mod;
void mod_settings_load(void);   // once (idempotent)
#define MLOG(...) do { if (g_mod.log) LOG("[mod] " __VA_ARGS__); } while (0)

// ---- guest memory helpers
static inline uint8_t g8(uint64_t a) { return *(volatile uint8_t *)(uintptr_t)a; }
static inline uint32_t g32(uint64_t a) { return *(volatile uint32_t *)(uintptr_t)a; }
static inline uint64_t g64(uint64_t a) { return *(volatile uint64_t *)(uintptr_t)a; }
static inline void s8(uint64_t a, uint8_t v) { *(volatile uint8_t *)(uintptr_t)a = v; }
static inline void s16(uint64_t a, uint16_t v) { *(volatile uint16_t *)(uintptr_t)a = v; }
static inline void s32(uint64_t a, uint32_t v) { *(volatile uint32_t *)(uintptr_t)a = v; }
static inline void s64(uint64_t a, uint64_t v) { *(volatile uint64_t *)(uintptr_t)a = v; }
// call a recompiled guest function directly (call-through for an overridden address: dispatch would come back here)
void guest_call_orig(Cpu *c, uint64_t addr);  // guest.c
// the network-session object (guest.c): OpenCourse World's play history is written into its user records
uint64_t netobj(void);
uint8_t wrapper_ok(void);       // the byte a wrapper returns on success (a game global)
void netobj_lock(Cpu *c);       // the lock the original wrappers take around their writes
void netobj_unlock(Cpu *c);
