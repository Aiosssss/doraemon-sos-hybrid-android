// Optional Atmosphere Cheat Engine + Built-in v1.1.2 Cheats + Live Equipped Tool Reader.
// Default state: 100% OFF (master = 0, every cheat = 0). Zero FPS cost when disabled.
#include "cheat_engine.h"
#ifndef CHEAT_ENGINE_HOST_TEST
#include "dm.h"
#include <mach/mach.h>
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef CHEAT_ENGINE_HOST_TEST
#define LOG(...) do { fprintf(stderr, "[cheat-test] "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
extern uint8_t *test_main_base(size_t *out_sz, size_t *out_text_end, const char **out_bid);
extern uint8_t *test_heap_base(size_t *out_sz);
extern uint64_t test_hid_buttons(void);
#else
extern int dm_guest_heap_readable(uint64_t a, size_t n);
extern uint8_t *dm_guest_heap_base(size_t *out_cur);
extern uint8_t *dm_guest_alias_base(size_t *out_cap);
extern uint8_t *dm_guest_as_base(size_t *out_cap);
extern uint64_t dm_hid_buttons_now(void);
#endif

// Verified main NSO build ID prefix for DORAEMON STORY OF SEASONS: Friends of the Great Kingdom v1.1.2
#define EXPECTED_BUILD_ID_16 "306dfcfc9eba4a1a"

#define CHEAT_MAX_ENTRIES   96
#define CHEAT_MAX_WORDS     256
#define CHEAT_MAX_BACKUPS   128
#define CHEAT_MAX_STACK     16

typedef struct {
    char id[64];
    char name[96];
    char source[64];     // "builtin" or filename
    int is_builtin;
    int group;           // mutual-exclusion group (1=friendship, 2=affection, 3=gold, 0=none)
    int enabled;
    // Built-in single-instruction surgical patch (v1.1.2 verified)
    uint64_t patch_rva;
    uint32_t orig_insn;
    uint32_t patch_insn;
    int dynamic_guard;   // 1 = dynamically armed/disarmed based on live held-item safety guard (cheat #12)
    // Imported Atmosphere VM bytecode words
    uint32_t *words;
    int nwords;
} cheat_entry_t;

typedef struct {
    uint64_t rva;
    uint32_t orig_word;
} code_backup_t;

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_master = 0;          // DEFAULT = 0 (OFF)
static _Atomic int g_any_enabled = 0;
static _Atomic int g_held_item_safe = 0;  // 1 only when holding a safe stackable Rucksack consumable
static int g_last_held_applied = 0;
static int g_inited = 0;
static int g_dirty_code = 0;

int dm_cheat_eval_held_item_guard(int sec_type, int32_t max_stack, int32_t item_id, int32_t count,
                                  uint64_t tool_ptr, int32_t master_tool_id, uint8_t is_important, int tool_ctx) {
    // 1. Must be in Rucksack (SectionType == 1), NEVER ToolPocket (2) or invalid section
    if (sec_type != 1) return 0;
    // 2. Never touch empty slots or zero/negative counts (prevents ghost items)
    if (item_id <= 0 || count <= 0) return 0;
    // 3. Reject non-stackable items/sections (maxStack <= 1) or out-of-range counts
    if (max_stack <= 1 || max_stack > 9999 || count > max_stack) return 0;
    // 4. Never touch Tools (ItemModel.mTool != NULL or ItemMasterModel.mToolId > 0)
    if (tool_ptr != 0 || master_tool_id > 0) return 0;
    // 5. Never touch Key / Quest Important Items (ItemMasterModel.mIsImportantItem != 0)
    if (is_important != 0) return 0;
    // 6. Never arm when Adaptive Button tool classifier detects any tool or animal-care implement
    if ((tool_ctx >= DM_TOOL_CTX_WATERING_CAN && tool_ctx <= DM_TOOL_CTX_BUG_NET) || tool_ctx == DM_TOOL_CTX_ANIMAL) return 0;
    return 1;
}
#ifdef CHEAT_ENGINE_HOST_TEST
void dm_cheat_test_set_held_safe(int safe) { atomic_store(&g_held_item_safe, safe ? 1 : 0); }
#else
static void refresh_game_context(void);
#endif
static char g_root[512];
static cheat_entry_t g_entries[CHEAT_MAX_ENTRIES];
static int g_nentries = 0;
static code_backup_t g_backups[CHEAT_MAX_BACKUPS];
static int g_nbackups = 0;

// ---- Main & Heap region accessors ----
static uint8_t *get_main_region(size_t *out_size, size_t *out_text_end, const char **out_bid) {
#ifdef CHEAT_ENGINE_HOST_TEST
    return test_main_base(out_size, out_text_end, out_bid);
#else
    for (int i = 0; i < g_nmod; i++) {
        if (!strcmp(g_mod[i].name, "main") && g_mod[i].base) {
            if (out_size) *out_size = g_mod[i].size;
            if (out_text_end) *out_text_end = (size_t)g_mod[i].text_end;
            if (out_bid) *out_bid = g_mod[i].build_id;
            return g_mod[i].base;
        }
    }
    return NULL;
#endif
}

static uint8_t *get_heap_region(size_t *out_size) {
#ifdef CHEAT_ENGINE_HOST_TEST
    return test_heap_base(out_size);
#else
    return dm_guest_heap_base(out_size);
#endif
}

static uint64_t get_hid_buttons(void) {
#ifdef CHEAT_ENGINE_HOST_TEST
    return test_hid_buttons();
#else
    return dm_hid_buttons_now();
#endif
}

// ---- Safe main .text patching with original-word backup & restore ----
static int write_main_text_u32(uint8_t *base, size_t text_end, uint64_t rva, uint32_t val, int record_backup) {
    if (!base || (rva & 3) || rva + 4 > text_end) return 0;
    uint32_t *ptr = (uint32_t *)(base + rva);
    uint32_t cur = *ptr;
    if (cur == val) return 1;

    if (record_backup) {
        int found = 0;
        for (int i = 0; i < g_nbackups; i++) {
            if (g_backups[i].rva == rva) { found = 1; break; }
        }
        if (!found && g_nbackups < CHEAT_MAX_BACKUPS) {
            g_backups[g_nbackups].rva = rva;
            g_backups[g_nbackups].orig_word = cur;
            g_nbackups++;
        }
    }

    long ps = sysconf(_SC_PAGESIZE);
    if (ps <= 0) ps = 4096;
    uintptr_t paddr = (uintptr_t)ptr & ~((uintptr_t)ps - 1);
    // Try RWX first (zero RX gap for concurrent guest threads); if Android SELinux / W^X denies RWX
    // (EACCES on vivo FuntouchOS / Samsung Knox / Android 13+), fall back to RW -> write -> clear_cache -> RX!
    int used_rw = 0;
    if (mprotect((void *)paddr, (size_t)ps, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        if (mprotect((void *)paddr, (size_t)ps, PROT_READ | PROT_WRITE) != 0) {
            LOG("[cheat] mprotect RWX & RW failed at main+%#llx (errno=%d)", (unsigned long long)rva, errno);
            return 0;
        }
        used_rw = 1;
    }
    *(volatile uint32_t *)ptr = val;
    __builtin___clear_cache((char *)ptr, (char *)ptr + 4);
    if (mprotect((void *)paddr, (size_t)ps, PROT_READ | PROT_EXEC) != 0) {
        LOG("[cheat] mprotect restore RX failed at main+%#llx (errno=%d)", (unsigned long long)rva, errno);
    }
    // Second I-cache flush after RX restore ensures ARM64 I-cache coherence even after W^X RW->RX transition.
    __builtin___clear_cache((char *)ptr, (char *)ptr + 4);
    uint32_t verify = *(const volatile uint32_t *)ptr;
    if (verify != val) {
        LOG("[cheat] readback mismatch at main+%#llx: got %#010x want %#010x", (unsigned long long)rva, verify, val);
        return 0;
    }
    if (used_rw) {
        LOG("[cheat] patched main+%#llx via W^X-safe RW->RX fallback (verified=%#010x)", (unsigned long long)rva, verify);
    }
    return 1;
}

static void restore_all_code_backups(void) {
    size_t msz = 0, text_end = 0;
    uint8_t *mbase = get_main_region(&msz, &text_end, NULL);
    if (!mbase) return;
    for (int i = 0; i < g_nbackups; i++) {
        write_main_text_u32(mbase, text_end, g_backups[i].rva, g_backups[i].orig_word, 0);
    }
    g_nbackups = 0;
}

// Validate a host address lies within safe mapped guest RAM (main RW data/bss, heap, or alias) or main .text
static int addr_region(uint64_t addr, size_t len, int is_write, int *out_is_text, uint64_t *out_main_rva) {
    if (out_is_text) *out_is_text = 0;
    if (out_main_rva) *out_main_rva = 0;
    if (!addr || len == 0 || len > 8) return 0;

    size_t msz = 0, text_end = 0;
    uint8_t *mbase = get_main_region(&msz, &text_end, NULL);
    if (mbase) {
        uint64_t mb = (uint64_t)(uintptr_t)mbase;
        if (addr >= mb && addr - mb + len <= msz) {
            uint64_t rva = addr - mb;
            if (rva < text_end) {
                if (out_is_text) *out_is_text = 1;
                if (out_main_rva) *out_main_rva = rva;
                return (!is_write) || ((rva & 3) == 0 && (len == 4 || len == 8));
            }
            return 1;
        }
    }
    size_t hsz = 0;
    uint8_t *hbase = get_heap_region(&hsz);
    if (hbase && hsz > 0) {
        uint64_t hb = (uint64_t)(uintptr_t)hbase;
        if (addr >= hb && addr - hb + len <= hsz) return 1;
    }
#ifndef CHEAT_ENGINE_HOST_TEST
    size_t asz = 0;
    uint8_t *abase = dm_guest_alias_base(&asz);
    if (abase && asz > 0) {
        uint64_t ab = (uint64_t)(uintptr_t)abase;
        if (addr >= ab && addr - ab + len <= asz) {
            // Only allow if readable in heap or check bounds conservatively
            return 0; // Alias region has sparse PROT_NONE holes; don't risk SIGSEGV
        }
    }
#endif
    return 0;
}

static int safe_read(uint64_t addr, int width, uint64_t *out) {
    if (!addr_region(addr, (size_t)width, 0, NULL, NULL)) return 0;
    uint64_t v = 0;
    memcpy(&v, (const void *)(uintptr_t)addr, (size_t)width);
    *out = v;
    return 1;
}

static int safe_write(uint64_t addr, int width, uint64_t val) {
    int is_text = 0;
    uint64_t rva = 0;
    if (!addr_region(addr, (size_t)width, 1, &is_text, &rva)) return 0;
    if (is_text) {
        size_t msz = 0, text_end = 0;
        uint8_t *mbase = get_main_region(&msz, &text_end, NULL);
        if (width == 4) {
            return write_main_text_u32(mbase, text_end, rva, (uint32_t)val, 1);
        } else if (width == 8) {
            int ok1 = write_main_text_u32(mbase, text_end, rva, (uint32_t)(val & 0xffffffffu), 1);
            int ok2 = write_main_text_u32(mbase, text_end, rva + 4, (uint32_t)(val >> 32), 1);
            return ok1 && ok2;
        }
        return 0;
    }
    memcpy((void *)(uintptr_t)addr, &val, (size_t)width);
    return 1;
}

static uint64_t memtype_base(int mtype) {
    size_t sz = 0;
    switch (mtype) {
        case 0: return (uint64_t)(uintptr_t)get_main_region(&sz, NULL, NULL);
        case 1: return (uint64_t)(uintptr_t)get_heap_region(&sz);
        case 2:
#ifndef CHEAT_ENGINE_HOST_TEST
            return (uint64_t)(uintptr_t)dm_guest_alias_base(&sz);
#else
            return 0;
#endif
        case 3:
#ifndef CHEAT_ENGINE_HOST_TEST
            return (uint64_t)(uintptr_t)dm_guest_as_base(&sz);
#else
            return (uint64_t)(uintptr_t)get_main_region(&sz, NULL, NULL);
#endif
        default: return 0;
    }
}

static int valid_width(int w) { return w == 1 || w == 2 || w == 4 || w == 8; }

static int cmp_vals(int cond, uint64_t lhs, uint64_t rhs) {
    switch (cond) {
        case 1: return lhs > rhs;
        case 2: return lhs >= rhs;
        case 3: return lhs < rhs;
        case 4: return lhs <= rhs;
        case 5: return lhs == rhs;
        case 6: return lhs != rhs;
        default: return 0;
    }
}

// ---- Atmosphere Cheat VM Executor ----
// Supports Code Types 0x0, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7, 0x8, 0x9, 0xA, 0xC0..0xC4
static void exec_atm_vm(const uint32_t *w, int n) {
    if (!w || n <= 0) return;
    uint64_t regs[16] = {0};
    uint64_t saved[16] = {0};
    uint32_t loop_cnt[16] = {0};
    int loop_pc[16] = {0};
    int cond_depth = 0;
    int skip_depth = 0;
    int safety_steps = 0;

    for (int pc = 0; pc < n && safety_steps < 4096; safety_steps++) {
        uint32_t op = w[pc++];
        unsigned code = (op >> 28) & 0xFu;

        // Handle conditional skipping (only Type 2 / Type 1 / Type 8 / Type C0 affect nesting)
        if (skip_depth > 0) {
            if (code == 0x2) {
                unsigned sub = (op >> 24) & 0xFu;
                if (sub == 1 && skip_depth == 1) {
                    // Else branch flips execution on!
                    skip_depth = 0;
                } else if (sub == 0) {
                    skip_depth--;
                    if (cond_depth > 0) cond_depth--;
                }
            } else if (code == 0x1 || code == 0x8 || (code == 0xC && ((op >> 24) & 0xFu) == 0)) {
                cond_depth++;
                skip_depth++;
                // Skip extra words of nested conditional
                if (code == 0x1) {
                    int width = (int)((op >> 24) & 0xFu);
                    pc += (width == 8) ? 3 : 2;
                } else if (code == 0xC) {
                    int width = (int)((op >> 20) & 0xFu);
                    int ot = (int)((op >> 8) & 0xFu);
                    if (ot == 0 || ot == 2) pc += (width == 8) ? 2 : 1;
                    else if (ot == 4) pc += (width == 8) ? 2 : 1;
                }
            } else if (code == 0x0) {
                int width = (int)((op >> 24) & 0xFu);
                pc += (width == 8) ? 3 : 2;
            } else if (code == 0x4) {
                pc += 2;
            } else if (code == 0x5) {
                pc += 1;
            } else if (code == 0x6) {
                pc += 2;
            } else if (code == 0x7) {
                pc += 1;
            } else if (code == 0x9) {
                if ((op & 0x0000F000u) == 0) {
                    int width = (int)((op >> 24) & 0xFu);
                    pc += (width == 8) ? 2 : 1;
                }
            } else if (code == 0xA) {
                unsigned mode = (op >> 12) & 0xFu;
                if (mode == 2 || mode == 4 || mode == 5) pc += 1;
            }
            continue;
        }

        switch (code) {
            case 0x0: { // 0TMR00AA AAAAAAAA VVVVVVVV [VVVVVVVV]: Store static value to memory
                int width = (int)((op >> 24) & 0xFu);
                int mtype = (int)((op >> 20) & 0xFu);
                int reg   = (int)((op >> 16) & 0xFu);
                if (!valid_width(width) || pc + (width == 8 ? 3 : 2) > n) return;
                uint64_t off = ((uint64_t)(op & 0xFFu) << 32) | (uint64_t)w[pc++];
                uint64_t val = (width == 8) ? (((uint64_t)w[pc] << 32) | (uint64_t)w[pc + 1]) : (uint64_t)w[pc];
                pc += (width == 8) ? 2 : 1;
                uint64_t base = memtype_base(mtype);
                if (base) safe_write(base + regs[reg] + off, width, val);
                break;
            }
            case 0x1: { // 1TMC00AA AAAAAAAA VVVVVVVV [VVVVVVVV]: Begin memory conditional block
                int width = (int)((op >> 24) & 0xFu);
                int mtype = (int)((op >> 20) & 0xFu);
                int cond  = (int)((op >> 16) & 0xFu);
                if (!valid_width(width) || pc + (width == 8 ? 3 : 2) > n) return;
                uint64_t off = ((uint64_t)(op & 0xFFu) << 32) | (uint64_t)w[pc++];
                uint64_t rhs = (width == 8) ? (((uint64_t)w[pc] << 32) | (uint64_t)w[pc + 1]) : (uint64_t)w[pc];
                pc += (width == 8) ? 2 : 1;
                uint64_t base = memtype_base(mtype), lhs = 0;
                int ok = base && safe_read(base + off, width, &lhs) && cmp_vals(cond, lhs, rhs);
                cond_depth++;
                if (!ok) skip_depth = 1;
                break;
            }
            case 0x2: { // 20000000 = Endif, 21000000 = Else
                unsigned sub = (op >> 24) & 0xFu;
                if (sub == 1) {
                    // We were executing the true branch, so skip the else branch
                    skip_depth = 1;
                } else if (cond_depth > 0) {
                    cond_depth--;
                }
                break;
            }
            case 0x3: { // 300R0000 VVVVVVVV (Start Loop) / 310R0000 (End Loop)
                unsigned is_end = (op >> 24) & 0xFu;
                int reg = (int)((op >> 16) & 0xFu);
                if (!is_end) {
                    if (pc >= n) return;
                    uint32_t cnt = w[pc++];
                    if (cnt > 256) cnt = 256;
                    loop_cnt[reg] = cnt;
                    loop_pc[reg] = pc;
                } else {
                    if (loop_cnt[reg] > 1) {
                        loop_cnt[reg]--;
                        pc = loop_pc[reg];
                    } else {
                        loop_cnt[reg] = 0;
                    }
                }
                break;
            }
            case 0x4: { // 400R0000 VVVVVVVV VVVVVVVV: Load register with 64-bit immediate
                int reg = (int)((op >> 16) & 0xFu);
                if (pc + 2 > n) return;
                uint64_t hi = w[pc++], lo = w[pc++];
                regs[reg] = (hi << 32) | lo;
                break;
            }
            case 0x5: { // 5TMR00AA AAAAAAAA: Load register from memory
                int width = (int)((op >> 24) & 0xFu);
                int mtype = (int)((op >> 20) & 0xFu);
                int reg   = (int)((op >> 16) & 0xFu);
                int from_reg = (int)((op >> 12) & 0xFu);
                if (!valid_width(width) || pc >= n) return;
                uint64_t off = ((uint64_t)(op & 0xFFu) << 32) | (uint64_t)w[pc++];
                uint64_t addr = (from_reg ? regs[reg] : memtype_base(mtype)) + off;
                uint64_t val = 0;
                if (addr && safe_read(addr, width, &val)) regs[reg] = val;
                else regs[reg] = 0;
                break;
            }
            case 0x6: { // 6T0RIor0 VVVVVVVV VVVVVVVV: Store static value to register address
                int width = (int)((op >> 24) & 0xFu);
                int reg   = (int)((op >> 16) & 0xFu);
                int inc   = (int)((op >> 12) & 0xFu);
                int add_o = (int)((op >> 8) & 0xFu);
                int oreg  = (int)((op >> 4) & 0xFu);
                if (!valid_width(width) || pc + 2 > n) return;
                uint64_t hi = w[pc++], lo = w[pc++];
                uint64_t val = (width == 8) ? ((hi << 32) | lo) : lo;
                uint64_t addr = regs[reg] + (add_o ? regs[oreg] : 0);
                if (addr) safe_write(addr, width, val);
                if (inc) regs[reg] += (uint64_t)width;
                break;
            }
            case 0x7: { // 7T0RC000 VVVVVVVV: Legacy arithmetic
                int width = (int)((op >> 24) & 0xFu);
                int reg   = (int)((op >> 16) & 0xFu);
                int aop   = (int)((op >> 12) & 0xFu);
                if (!valid_width(width) || pc >= n) return;
                uint64_t imm = w[pc++];
                uint64_t mask = (width == 8) ? ~0ull : ((1ull << (width * 8)) - 1ull);
                uint64_t v = regs[reg] & mask;
                if (aop == 0) v += imm;
                else if (aop == 1) v -= imm;
                else if (aop == 2) v *= imm;
                else if (aop == 3) v <<= (imm & 63);
                else if (aop == 4) v >>= (imm & 63);
                regs[reg] = v & mask;
                break;
            }
            case 0x8: { // 8kkkkkkk: Begin keypress conditional block
                uint32_t want = op & 0x0FFFFFFFu;
                uint64_t held = get_hid_buttons();
                int ok = ((held & want) == want);
                cond_depth++;
                if (!ok) skip_depth = 1;
                break;
            }
            case 0x9: { // 9TCRS0s0 [VVVVVVVV [VVVVVVVV]]: Arithmetic
                int width = (int)((op >> 24) & 0xFu);
                int aop   = (int)((op >> 20) & 0xFu);
                int dst   = (int)((op >> 16) & 0xFu);
                int src1  = (int)((op >> 12) & 0xFu);
                int is_imm = (((op >> 8) & 0xFu) != 0);
                int src2  = (int)((op >> 4) & 0xFu);
                if (!valid_width(width)) return;
                uint64_t mask = (width == 8) ? ~0ull : ((1ull << (width * 8)) - 1ull);
                uint64_t rhs = 0;
                if (is_imm) {
                    if (pc + (width == 8 ? 2 : 1) > n) return;
                    rhs = (width == 8) ? (((uint64_t)w[pc] << 32) | w[pc + 1]) : w[pc];
                    pc += (width == 8) ? 2 : 1;
                } else {
                    rhs = regs[src2] & mask;
                }
                uint64_t lhs = regs[src1] & mask, res = lhs;
                switch (aop) {
                    case 0: res = lhs + rhs; break;
                    case 1: res = lhs - rhs; break;
                    case 2: res = lhs * rhs; break;
                    case 3: res = lhs << (rhs & 63); break;
                    case 4: res = lhs >> (rhs & 63); break;
                    case 5: res = lhs & rhs; break;
                    case 6: res = lhs | rhs; break;
                    case 7: res = ~lhs; break;
                    case 8: res = lhs ^ rhs; break;
                    case 9: res = is_imm ? rhs : lhs; break;
                }
                regs[dst] = res & mask;
                break;
            }
            case 0xA: { // ATSRIOa0 [AAAAAAAA]: Store register to memory address
                int width = (int)((op >> 24) & 0xFu);
                int sreg  = (int)((op >> 20) & 0xFu);
                int areg  = (int)((op >> 16) & 0xFu);
                int inc   = (int)((op >> 12) & 0xFu);
                int mode  = (int)((op >> 8) & 0xFu);
                int oreg  = (int)((op >> 4) & 0xFu);
                if (!valid_width(width)) return;
                uint64_t addr = 0;
                if (mode == 0) addr = regs[areg];
                else if (mode == 1) addr = regs[areg] + regs[oreg];
                else if (mode == 2) { if (pc >= n) return; addr = regs[areg] + w[pc++]; }
                else if (mode == 3) addr = memtype_base(oreg) + regs[areg];
                else if (mode == 4) { if (pc >= n) return; addr = memtype_base(oreg) + w[pc++]; }
                else if (mode == 5) { if (pc >= n) return; addr = memtype_base(oreg) + regs[areg] + w[pc++]; }
                if (addr) safe_write(addr, width, regs[sreg]);
                if (inc) regs[areg] += (uint64_t)width;
                break;
            }
            case 0xC: {
                unsigned sub = (op >> 24) & 0xFu;
                if (sub == 0) { // C0TcS0X0 ... Register conditional
                    int width = (int)((op >> 20) & 0xFu);
                    int cond  = (int)((op >> 16) & 0xFu);
                    int sreg  = (int)((op >> 12) & 0xFu);
                    int otype = (int)((op >> 8) & 0xFu);
                    int mtype = (int)((op >> 4) & 0xFu);
                    if (!valid_width(width)) return;
                    uint64_t mask = (width == 8) ? ~0ull : ((1ull << (width * 8)) - 1ull);
                    uint64_t lhs = regs[sreg] & mask, rhs = 0;
                    int ok = 0;
                    if (otype == 4) {
                        if (pc + (width == 8 ? 2 : 1) > n) return;
                        rhs = (width == 8) ? (((uint64_t)w[pc] << 32) | w[pc + 1]) : w[pc];
                        pc += (width == 8) ? 2 : 1;
                        ok = cmp_vals(cond, lhs, rhs & mask);
                    } else if (otype == 5) {
                        rhs = regs[mtype] & mask;
                        ok = cmp_vals(cond, lhs, rhs);
                    } else {
                        if (otype == 0 || otype == 2) { if (pc >= n) return; }
                        uint64_t off = (otype == 0 || otype == 2) ? w[pc++] : regs[op & 0xFu];
                        uint64_t base = (otype <= 1) ? memtype_base(mtype) : regs[mtype];
                        ok = base && safe_read(base + off, width, &rhs) && cmp_vals(cond, lhs, rhs & mask);
                    }
                    cond_depth++;
                    if (!ok) skip_depth = 1;
                } else if (sub == 1) { // C10D0S0x Save/Restore register
                    int dst = (int)((op >> 16) & 0xFu);
                    int src = (int)((op >> 8) & 0xFu);
                    int mode = (int)(op & 0xFu);
                    if (mode == 0) regs[dst] = saved[src];
                    else if (mode == 1) saved[dst] = regs[src];
                    else if (mode == 2) saved[dst] = 0;
                    else if (mode == 3) regs[dst] = 0;
                }
                break;
            }
            default:
                break;
        }
    }
}

// ---- Built-in v1.1.2 Cheats (ALL verified directly from main build 306dfcfc9eba4a1a) ----
static void add_builtin(const char *id, const char *name, int group, uint64_t rva, uint32_t orig, uint32_t patch) {
    if (g_nentries >= CHEAT_MAX_ENTRIES) return;
    cheat_entry_t *e = &g_entries[g_nentries++];
    memset(e, 0, sizeof *e);
    snprintf(e->id, sizeof e->id, "%s", id);
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->source, sizeof e->source, "builtin");
    e->is_builtin = 1;
    e->group = group;
    e->enabled = 0; // ALWAYS OFF by default!
    e->patch_rva = rva;
    e->orig_insn = orig;
    e->patch_insn = patch;
}

static void register_builtins(void) {
    // 1. Villager Friendship 2x / 4x (NpcModel::AddLikabilityDegree @ 0x03b70a44: add w0, w23, w21 -> lsl #1 / #2)
    add_builtin("builtin_friend_2x",  "Friendship Warga 2x",     1, 0x03b70a44ull, 0x0b1502e0u, 0x0b1506e0u);
    add_builtin("builtin_friend_4x",  "Friendship Warga 4x",     1, 0x03b70a44ull, 0x0b1502e0u, 0x0b150ae0u);
    // 2. Animal Affection 2x / 4x (AnimalModel::AddAffection @ 0x03b8fec4: add w1, w8, w20 -> lsl #1 / #2)
    add_builtin("builtin_animal_2x",  "Affection Hewan 2x",      2, 0x03b8fec4ull, 0x0b140101u, 0x0b140501u);
    add_builtin("builtin_animal_4x",  "Affection Hewan 4x",      2, 0x03b8fec4ull, 0x0b140101u, 0x0b140901u);
    // 3. Infinite Stamina (StaminaModel::Consume @ 0x03cab7f8: sub w0, w23, w22 -> mov w0, w23)
    add_builtin("builtin_stamina",    "Stamina Tak Terbatas",    0, 0x03cab7f8ull, 0x4b1602e0u, 0x2a1703e0u);
    // 4. Infinite Watering Can (ItemModel::Use @ 0x03c366e0: sub w8, w8, #1 -> nop)
    add_builtin("builtin_water",      "Air Penyiram Tak Habis",  0, 0x03c366e0ull, 0x51000508u, 0xd503201fu);
    // 5. Freeze Time (TimeModel::AdvanceMinute @ 0x01bef750: mov w1, #1 -> ret)
    add_builtin("builtin_freeze_time","Waktu Berhenti",          0, 0x01bef750ull, 0x52800021u, 0xd65f03c0u);
    // 6. Gold / Money 2x / 4x (PlayerModel::AddMoney @ 0x01a0b74c: add w1, w8, w20 -> lsl #1 / #2)
    add_builtin("builtin_gold_2x",    "Gold / Uang 2x",          3, 0x01a0b74cull, 0x0b140101u, 0x0b140501u);
    add_builtin("builtin_gold_4x",    "Gold / Uang 4x",          3, 0x01a0b74cull, 0x0b140101u, 0x0b140901u);
    // 7. Walk & Run Speed 1.5x / 2.0x (FarmPlayerController::ConvertRateToSpeed @ 0x03dd1dac:
    //    orig 0x52a85909 = mov w9, #0x42c80000 (100.0f) -> 66.5f (1.504x) / 50.0f (2.000x))
    add_builtin("builtin_speed_15x",  "Kecepatan Jalan/Lari 1.5x", 4, 0x03dd1dacull, 0x52a85909u, 0x52a850a9u);
    add_builtin("builtin_speed_20x",  "Kecepatan Jalan/Lari 2.0x", 4, 0x03dd1dacull, 0x52a85909u, 0x52a84909u);
    // 8. Infinite Held Item in Active Slot (InventorySectionModel::TryReduceItemInHand @ 0x03df4d2c:
    //    orig 0x4b170129 = sub w9, w9, w23 -> 0xd503201f = nop; dynamically guarded for safe stackable Rucksack items!)
    add_builtin("builtin_inf_held_item", "Item Tak Habis (Slot Dipegang)", 0, 0x03df4d2cull, 0x4b170129u, 0xd503201fu);
    if (g_nentries > 0) g_entries[g_nentries - 1].dynamic_guard = 1;
}

// ---- Atmosphere .txt Cheat File Parser ----
static int parse_hex_u32(const char *s, uint32_t *out) {
    if (strlen(s) != 8) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)s[i];
        uint32_t d;
        if (c >= '0' && c <= '9') d = (uint32_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint32_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint32_t)(c - 'A' + 10);
        else return 0;
        v = (v << 4) | d;
    }
    *out = v;
    return 1;
}

static void flush_atm_entry(cheat_entry_t **pcur, uint32_t *tmp, int *pntmp) {
    cheat_entry_t *cur = *pcur;
    int ntmp = *pntmp;
    if (!cur) return;
    if (ntmp > 0) {
        cur->words = (uint32_t *)malloc((size_t)ntmp * sizeof(uint32_t));
        if (cur->words) {
            memcpy(cur->words, tmp, (size_t)ntmp * sizeof(uint32_t));
            cur->nwords = ntmp;
        }
    } else if (g_nentries > 0) {
        g_nentries--;
    }
    *pcur = NULL;
    *pntmp = 0;
}

static void load_atm_file(const char *path, const char *fname) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    cheat_entry_t *cur = NULL;
    uint32_t tmp[CHEAT_MAX_WORDS];
    int ntmp = 0;
    int idx_in_file = 0;

    while (fgets(line, sizeof line, f)) {
        // Strip UTF-8 BOM if present
        char *p = line;
        if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
        while (*p && isspace((unsigned char)*p)) p++;
        size_t len = strlen(p);
        while (len > 0 && isspace((unsigned char)p[len - 1])) p[--len] = 0;
        if (len == 0 || p[0] == '#' || p[0] == ';') continue;

        if ((p[0] == '[' && p[len - 1] == ']') || (p[0] == '{' && p[len - 1] == '}')) {
            flush_atm_entry(&cur, tmp, &ntmp);
            if (g_nentries >= CHEAT_MAX_ENTRIES) break;
            cur = &g_entries[g_nentries++];
            memset(cur, 0, sizeof *cur);
            p[len - 1] = 0;
            snprintf(cur->id, sizeof cur->id, "atm:%s:%d", fname, idx_in_file++);
            snprintf(cur->name, sizeof cur->name, "%s", p + 1);
            snprintf(cur->source, sizeof cur->source, "%s", fname);
            cur->is_builtin = 0;
            cur->enabled = 0;
            ntmp = 0;
            continue;
        }
        if (!cur) continue;
        // Parse space-separated 8-hex-digit words
        char *tok = strtok(p, " \t");
        while (tok && ntmp < CHEAT_MAX_WORDS) {
            uint32_t w = 0;
            if (parse_hex_u32(tok, &w)) tmp[ntmp++] = w;
            tok = strtok(NULL, " \t");
        }
    }
    flush_atm_entry(&cur, tmp, &ntmp);
    fclose(f);
}

// ---- Persistence ($DM_ROOT/cheats_state.tsv) ----
static void state_load_locked(void) {
    if (!g_root[0]) return;
    char path[600];
    snprintf(path, sizeof path, "%s/cheats_state.tsv", g_root);
    FILE *f = fopen(path, "r");
    int master = 0;
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            char *nl = strpbrk(line, "\r\n");
            if (nl) *nl = 0;
            if (!strncmp(line, "master=", 7)) {
                master = atoi(line + 7) != 0;
            } else if (!strncmp(line, "on=", 3)) {
                const char *id = line + 3;
                for (int i = 0; i < g_nentries; i++) {
                    if (!strcmp(g_entries[i].id, id)) {
                        g_entries[i].enabled = 1;
                        break;
                    }
                }
            }
        }
        fclose(f);
    }
    const char *env = getenv("DM_CHEATS");
    if (env && env[0]) master = atoi(env) != 0;
    atomic_store(&g_master, master);
}

static void state_save_locked(void) {
    if (!g_root[0]) return;
    char path[600], tmp[612];
    snprintf(path, sizeof path, "%s/cheats_state.tsv", g_root);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "master=%d\n", atomic_load(&g_master) ? 1 : 0);
    for (int i = 0; i < g_nentries; i++) {
        if (g_entries[i].enabled) fprintf(f, "on=%s\n", g_entries[i].id);
    }
    fclose(f);
    rename(tmp, path);
}

static void recompute_active_locked(void) {
    int any = 0;
    for (int i = 0; i < g_nentries; i++) {
        if (g_entries[i].enabled) { any = 1; break; }
    }
    atomic_store(&g_any_enabled, any);
    g_dirty_code = 1;
}

static int apply_builtin_patches_locked(void) {
    // Always restore first so disabled cheats return cleanly to exact original instructions
    restore_all_code_backups();
    g_last_held_applied = 0;
    if (!atomic_load(&g_master)) return 1;

    size_t msz = 0, text_end = 0;
    const char *bid = NULL;
    uint8_t *mbase = get_main_region(&msz, &text_end, &bid);
    if (!mbase) return 0;   // main not loaded yet -> keep g_dirty_code=1 so next tick retries!
    int bid_ok = (bid && strncasecmp(bid, EXPECTED_BUILD_ID_16, 16) == 0);

    for (int i = 0; i < g_nentries; i++) {
        cheat_entry_t *e = &g_entries[i];
        if (!e->is_builtin || !e->enabled) continue;
        if (!bid_ok) {
            LOG("[cheat] skip built-in '%s': main build_id '%s' != %s", e->name, bid ? bid : "?", EXPECTED_BUILD_ID_16);
            continue;
        }
        if (e->dynamic_guard) {
            int safe = atomic_load(&g_held_item_safe);
            g_last_held_applied = safe;
            if (!safe) continue;
        }
        if (e->patch_rva + 4 > text_end) continue;
        uint32_t cur = *(const uint32_t *)(mbase + e->patch_rva);
        if (cur != e->orig_insn && cur != e->patch_insn) {
            LOG("[cheat] skip built-in '%s': unexpected insn %#010x at main+%#llx (want %#010x)",
                e->name, cur, (unsigned long long)e->patch_rva, e->orig_insn);
            continue;
        }
        if (write_main_text_u32(mbase, text_end, e->patch_rva, e->patch_insn, 1)) {
            LOG("[cheat] applied built-in '%s' @ main+%#llx (%#010x -> %#010x)",
                e->name, (unsigned long long)e->patch_rva, e->orig_insn, e->patch_insn);
        }
    }
    return 1;
}

static void reload_locked(void) {
    restore_all_code_backups();
    for (int i = 0; i < g_nentries; i++) {
        free(g_entries[i].words);
        g_entries[i].words = NULL;
    }
    g_nentries = 0;
    register_builtins();

    if (g_root[0]) {
        char cdir[600];
        snprintf(cdir, sizeof cdir, "%s/cheats", g_root);
        DIR *d = opendir(cdir);
        if (d) {
            struct dirent *de;
            while ((de = readdir(d)) != NULL) {
                size_t nl = strlen(de->d_name);
                if (nl < 5 || strcasecmp(de->d_name + nl - 4, ".txt") != 0) continue;
                char fpath[768];
                snprintf(fpath, sizeof fpath, "%s/%s", cdir, de->d_name);
                load_atm_file(fpath, de->d_name);
            }
            closedir(d);
        }
    }
    state_load_locked();
    recompute_active_locked();
    if (apply_builtin_patches_locked()) g_dirty_code = 0;
    LOG("[cheat] loaded %d cheats (%d built-in), master=%d", g_nentries, 12, atomic_load(&g_master));
}

void dm_cheats_init(const char *root_dir) {
    pthread_mutex_lock(&g_mu);
    if (root_dir && root_dir[0]) snprintf(g_root, sizeof g_root, "%s", root_dir);
    else {
        const char *r = getenv("DM_ROOT");
        if (r && r[0]) snprintf(g_root, sizeof g_root, "%s", r);
    }
    g_inited = 1;
    reload_locked();
    pthread_mutex_unlock(&g_mu);
}

void dm_cheats_reload(void) {
    pthread_mutex_lock(&g_mu);
    if (!g_inited) {
        const char *r = getenv("DM_ROOT");
        if (r && r[0]) snprintf(g_root, sizeof g_root, "%s", r);
        g_inited = 1;
    }
    reload_locked();
    pthread_mutex_unlock(&g_mu);
}

void dm_cheats_tick(void) {
    // Fast path: 1 atomic load when master == 0 and no dirty restore pending
    if (!g_inited) {
        const char *r = getenv("DM_ROOT");
        if (r && r[0]) dm_cheats_init(r);
        else return;
    }
    if (!atomic_load(&g_master) && !g_dirty_code) return;

    pthread_mutex_lock(&g_mu);
    if (g_dirty_code) {
        if (apply_builtin_patches_locked()) g_dirty_code = 0;
    }
    if (atomic_load(&g_master) && atomic_load(&g_any_enabled)) {
        int dyn_enabled = 0;
        for (int i = 0; i < g_nentries; i++) {
            cheat_entry_t *e = &g_entries[i];
            if (e->is_builtin && e->dynamic_guard && e->enabled) {
                dyn_enabled = 1;
            } else if (!e->is_builtin && e->enabled && e->words && e->nwords > 0) {
                exec_atm_vm(e->words, e->nwords);
            }
        }
        if (dyn_enabled) {
#ifndef CHEAT_ENGINE_HOST_TEST
            // Share lightweight 10 Hz context walk with Adaptive Button (zero-cost when cheat OFF!)
            refresh_game_context();
#endif
            int safe = atomic_load(&g_held_item_safe);
            if (safe != g_last_held_applied) {
                size_t msz = 0, text_end = 0;
                const char *bid = NULL;
                uint8_t *mbase = get_main_region(&msz, &text_end, &bid);
                if (mbase && bid && strncasecmp(bid, EXPECTED_BUILD_ID_16, 16) == 0) {
                    for (int i = 0; i < g_nentries; i++) {
                        cheat_entry_t *e = &g_entries[i];
                        if (!e->is_builtin || !e->dynamic_guard || !e->enabled) continue;
                        if (e->patch_rva + 4 > text_end) continue;
                        uint32_t cur = *(const uint32_t *)(mbase + e->patch_rva);
                        if (cur != e->orig_insn && cur != e->patch_insn) continue;
                        uint32_t want = safe ? e->patch_insn : e->orig_insn;
                        if (write_main_text_u32(mbase, text_end, e->patch_rva, want, 1)) {
                            g_last_held_applied = safe;
                        }
                    }
                }
            }
        }
    }
    pthread_mutex_unlock(&g_mu);
}

int dm_cheats_master(void) { return atomic_load(&g_master); }

void dm_cheats_set_master(int on) {
    pthread_mutex_lock(&g_mu);
    atomic_store(&g_master, on ? 1 : 0);
    g_dirty_code = apply_builtin_patches_locked() ? 0 : 1;
    state_save_locked();
    LOG("[cheat] master=%d", on ? 1 : 0);
    pthread_mutex_unlock(&g_mu);
}

int dm_cheats_count(void) {
    pthread_mutex_lock(&g_mu);
    int n = g_nentries;
    pthread_mutex_unlock(&g_mu);
    return n;
}

int dm_cheats_info(int idx, char *name_out, size_t nsz, int *enabled_out, int *is_builtin_out) {
    pthread_mutex_lock(&g_mu);
    if (idx < 0 || idx >= g_nentries) { pthread_mutex_unlock(&g_mu); return 0; }
    if (name_out && nsz > 0) snprintf(name_out, nsz, "%s", g_entries[idx].name);
    if (enabled_out) *enabled_out = g_entries[idx].enabled;
    if (is_builtin_out) *is_builtin_out = g_entries[idx].is_builtin;
    pthread_mutex_unlock(&g_mu);
    return 1;
}

void dm_cheats_toggle(int idx) {
    pthread_mutex_lock(&g_mu);
    if (idx >= 0 && idx < g_nentries) {
        int next = !g_entries[idx].enabled;
        if (next && g_entries[idx].group != 0) {
            int grp = g_entries[idx].group;
            for (int i = 0; i < g_nentries; i++) {
                if (i != idx && g_entries[i].group == grp) g_entries[i].enabled = 0;
            }
        }
        g_entries[idx].enabled = next;
        // Turning on any cheat while master is OFF auto-enables master for UX convenience
        if (next && !atomic_load(&g_master)) atomic_store(&g_master, 1);
        recompute_active_locked();
        if (apply_builtin_patches_locked()) g_dirty_code = 0;
        state_save_locked();
        LOG("[cheat] '%s' -> %s (master=%d)", g_entries[idx].name, next ? "ON" : "OFF", atomic_load(&g_master));
    }
    pthread_mutex_unlock(&g_mu);
}

// ---- Live Game Equipped Tool & Situation Context Reader (for Adaptive Button Level 2) ----
// Verified v1.1.2 (306dfcfc9eba4a1a) read-only pointer walk guarded by vm_read_overwrite() so it works
// across g_heap, g_alias, mmap, AND IL2CPP Boehm GC allocations with 0% crash risk!
#ifndef CHEAT_ENGINE_HOST_TEST
static int safe_vm_read(uint64_t addr, void *out, size_t n) {
    if (addr < 0x100000ull || addr > 0x0001000000000000ull - n) return 0;
    vm_size_t got = 0;
    return vm_read_overwrite(mach_task_self(), (vm_address_t)addr, n, (vm_address_t)out, &got) == KERN_SUCCESS && got == n;
}
static int safe_vm_u64(uint64_t addr, uint64_t *v) { return (addr & 7ull) == 0 && safe_vm_read(addr, v, 8); }
static int safe_vm_i32(uint64_t addr, int32_t *v)  { return (addr & 3ull) == 0 && safe_vm_read(addr, v, 4); }
static int safe_vm_u8(uint64_t addr, uint8_t *v)   { return safe_vm_read(addr, v, 1); }

// IL2CPP System.Collections.Generic.HashSet<int> reader (verified from main.patched.bin @ 0x34c4220):
//   hs + 0x18 = Slot[] _slots (Il2CppArray header 0x20, length at +0x18)
//   Slot = { int32_t hashCode; int32_t next; int32_t value; } (12 bytes each; hashCode >= 0 => occupied)
static int il2cpp_hashset_int_contains(uint64_t hs, int32_t needle) {
    if (!hs || (hs & 7ull)) return 0;
    uint64_t slots = 0;
    if (!safe_vm_u64(hs + 0x18, &slots) || !slots || (slots & 7ull)) return 0;
    int32_t cap = 0;
    if (!safe_vm_i32(slots + 0x18, &cap) || cap <= 0) return 0;
    if (cap > 128) cap = 128;
    struct { int32_t hash_code; int32_t next; int32_t value; } buf[128];
    if (!safe_vm_read(slots + 0x20, buf, (size_t)cap * 12)) return 0;
    for (int32_t i = 0; i < cap; i++) {
        if (buf[i].hash_code >= 0 && buf[i].value == needle) return 1;
    }
    return 0;
}

static _Atomic uint64_t g_ctx_last_ns = 0;
static _Atomic int g_ctx_tool = DM_TOOL_CTX_HAND;
static _Atomic int g_ctx_interact = DM_INTERACT_CTX_CONFIRM;

static void refresh_game_context(void) {
    uint64_t now = now_ns();
    uint64_t prev = atomic_load(&g_ctx_last_ns);
    if (now - prev < 100000000ull) return;   // Throttled to 10 Hz (~1x per 6 frames at 60 FPS)
    atomic_store(&g_ctx_last_ns, now);

    int tool = DM_TOOL_CTX_HAND;
    int inter = DM_INTERACT_CTX_CONFIRM;
    int32_t item_id = 0, sec_type = 0, qidx = -1, item_count = 0, max_stack = 0, master_tool_id = 0;
    uint64_t m_tool = 0, m_master = 0;
    uint8_t carrying = 0, is_important = 0;
    int held_safe = 0;

    size_t msz = 0, text_end = 0;
    const char *bid = NULL;
    uint8_t *mbase = get_main_region(&msz, &text_end, &bid);
    if (!mbase || !bid || strncasecmp(bid, EXPECTED_BUILD_ID_16, 16) != 0 || msz < 0x6700000ull) goto done;

    // Walk SingletonMonoBehaviour<UserManager>::get_Instance() MethodInfo -> static_fields -> UserManager -> UserModel -> InventoryModel
    uint64_t got_um = 0, mi = 0, klass = 0, rgctx = 0, sklass = 0, sfields = 0, user_mgr = 0;
    if (!safe_vm_u64((uint64_t)(uintptr_t)(mbase + 0x66265b8), &got_um) || !got_um) goto done;
    if (!safe_vm_u64(got_um, &mi) || !mi) goto done;
    if (!safe_vm_u64(mi + 0x18, &klass) || !klass) goto done;
    if (!safe_vm_u64(klass + 0xc0, &rgctx) || !rgctx) goto done;
    if (!safe_vm_u64(rgctx, &sklass) || !sklass) goto done;
    if (!safe_vm_u64(sklass + 0xb8, &sfields) || !sfields) goto done;
    if (!safe_vm_u64(sfields, &user_mgr) || !user_mgr) goto done;

    uint64_t user_model = 0, inv_model = 0;
    if (!safe_vm_u64(user_mgr + 0x18, &user_model) || !user_model) goto done;
    if (!safe_vm_u64(user_model + 0x30, &inv_model) || !inv_model) goto done;

    // Active gameplay detected! Default A button switches from Confirm -> Talk/Interact.
    inter = DM_INTERACT_CTX_TALK;

    safe_vm_i32(inv_model + 0x20, &sec_type);
    safe_vm_u8(inv_model + 0x28, &carrying);
    if (carrying) {
        inter = DM_INTERACT_CTX_HARVEST;
        tool = DM_TOOL_CTX_CARRY;
    }

    // SectionType: 1 = Rucksack (+0x10), 2 = ToolPocket (+0x18)
    uint64_t sec = 0;
    if (!safe_vm_u64(inv_model + (sec_type == 1 ? 0x10 : 0x18), &sec) || !sec) goto done;
    safe_vm_i32(sec + 0x28, &max_stack);   // ItemSlotModel::mMaxStack (+0x28, verified @ 0x03c3cd20)

    uint64_t qslots = 0;
    if (!safe_vm_u64(sec + 0x30, &qslots) || !qslots) goto done;
    if (!safe_vm_i32(sec + 0x38, &qidx)) goto done;
    int32_t qlen = 0;
    if (!safe_vm_i32(qslots + 0x18, &qlen) || qidx < 0 || qidx >= qlen || qlen > 64) goto done;

    uint64_t item = 0;
    if (!safe_vm_u64(qslots + 0x20 + (uint64_t)qidx * 8, &item) || !item) goto done;
    if (!safe_vm_i32(item + 0x10, &item_id) || item_id <= 0) goto done;
    safe_vm_i32(item + 0x18, &item_count); // ItemModel::mCount (+0x18, verified @ 0x03c35310)
    safe_vm_u64(item + 0x48, &m_master);   // ItemModel::mMaster (+0x48, verified @ 0x03c35a8c)
    safe_vm_u64(item + 0x50, &m_tool);     // ItemModel::mTool (+0x50, verified @ 0x03c35570)
    if (m_master) {
        safe_vm_u8(m_master + 0x1e, &is_important);    // ItemMasterModel::mIsImportantItem (+0x1e @ 0x03c351d0)
        safe_vm_i32(m_master + 0x34, &master_tool_id); // ItemMasterModel::mToolId (+0x34 @ 0x03c35240)
    }

    // Seed range check from Define.Item::IsSeed (0x03dfbd54): ((uint32_t)(item_id - 2000000) >> 5) < 0xc35
    if ((((uint32_t)item_id + 0xFFE17B80u) >> 5) < 0xC35u) {
        tool = DM_TOOL_CTX_SEEDS;
        goto done;
    }

    // Match against verified Define.Item static HashSet<int> fields (main + 0x6626770 -> *slot -> +0xb8)
    int matched = 0;
    uint64_t got_di = 0, di_klass = 0, di_sf = 0;
    if (safe_vm_u64((uint64_t)(uintptr_t)(mbase + 0x6626770), &got_di) && got_di &&
        safe_vm_u64(got_di, &di_klass) && di_klass &&
        safe_vm_u64(di_klass + 0xb8, &di_sf) && di_sf) {
        uint64_t hs = 0;
        if (safe_vm_u64(di_sf + 0x460, &hs) && il2cpp_hashset_int_contains(hs, item_id))      { tool = DM_TOOL_CTX_WATERING_CAN; matched = 1; }
        else if (safe_vm_u64(di_sf + 0x450, &hs) && il2cpp_hashset_int_contains(hs, item_id)) { tool = DM_TOOL_CTX_HOE; matched = 1; }
        else if ((safe_vm_u64(di_sf + 0x458, &hs) && il2cpp_hashset_int_contains(hs, item_id)) ||
                 (safe_vm_u64(di_sf + 0x478, &hs) && il2cpp_hashset_int_contains(hs, item_id))) { tool = DM_TOOL_CTX_HAMMER; matched = 1; }
        else if (safe_vm_u64(di_sf + 0x470, &hs) && il2cpp_hashset_int_contains(hs, item_id)) { tool = DM_TOOL_CTX_AXE; matched = 1; }
        else if (safe_vm_u64(di_sf + 0x468, &hs) && il2cpp_hashset_int_contains(hs, item_id)) { tool = DM_TOOL_CTX_SICKLE; matched = 1; }
        else if (safe_vm_u64(di_sf + 0x480, &hs) && il2cpp_hashset_int_contains(hs, item_id)) { tool = DM_TOOL_CTX_FISHING_ROD; matched = 1; }
        else if (safe_vm_u64(di_sf + 0x4a0, &hs) && il2cpp_hashset_int_contains(hs, item_id)) { tool = DM_TOOL_CTX_BUG_NET; matched = 1; }
        else if (safe_vm_u64(di_sf + 0x4a8, &hs) && il2cpp_hashset_int_contains(hs, item_id)) { tool = DM_TOOL_CTX_SEEDS; matched = 1; }
        else if ((safe_vm_u64(di_sf + 0x4b0, &hs) && il2cpp_hashset_int_contains(hs, item_id)) ||
                 (safe_vm_u64(di_sf + 0x4b8, &hs) && il2cpp_hashset_int_contains(hs, item_id)) ||
                 (safe_vm_u64(di_sf + 0x4c0, &hs) && il2cpp_hashset_int_contains(hs, item_id)) ||
                 (safe_vm_u64(di_sf + 0x540, &hs) && il2cpp_hashset_int_contains(hs, item_id))) {
            tool = DM_TOOL_CTX_ANIMAL; matched = 1;
        } else {
            // Check scalar Define.Item animal-care tools (ID_BRUSH / ID_MILKER / ID_SHEARS stored sequentially before ID_BUG_CATCHING_NET)
            // Fallback: check ItemModel.mTool != null in ToolPocket for brush/milker/shears
            int32_t id_brush = 0, id_milker = 0, id_shears = 0;
            if ((safe_vm_i32(di_sf + 0x160, &id_brush) && id_brush == item_id) ||
                (safe_vm_i32(di_sf + 0x164, &id_milker) && id_milker == item_id) ||
                (safe_vm_i32(di_sf + 0x168, &id_shears) && id_shears == item_id)) {
                tool = DM_TOOL_CTX_ANIMAL; matched = 1;
            }
        }
    }
    if (!matched && sec_type == 1 && item_id > 0) {
        // Holding an edible crop / food / gift in the Rucksack
        tool = DM_TOOL_CTX_CARRY;
    }

done:
    held_safe = dm_cheat_eval_held_item_guard(sec_type, max_stack, item_id, item_count, m_tool, master_tool_id, is_important, tool);
    atomic_store(&g_held_item_safe, held_safe);
    {
        static int prev_tool = -1, prev_inter = -1, prev_item = -1, log_cnt = 0;
        if ((tool != prev_tool || inter != prev_inter || item_id != prev_item) && log_cnt < 64) {
            prev_tool = tool; prev_inter = inter; prev_item = item_id; log_cnt++;
            LOG("[adaptive] ctx: tool=%d interact=%d sec=%d qidx=%d carry=%u itemId=%d count=%d maxStack=%d safe=%d",
                tool, inter, sec_type, qidx, (unsigned)carrying, item_id, item_count, max_stack, held_safe);
        }
    }
    atomic_store(&g_ctx_tool, tool);
    atomic_store(&g_ctx_interact, inter);
}

int dm_game_tool_context(void) {
    refresh_game_context();
    return atomic_load(&g_ctx_tool);
}

int dm_game_interact_context(void) {
    refresh_game_context();
    return atomic_load(&g_ctx_interact);
}
#else
int dm_game_tool_context(void) { return DM_TOOL_CTX_HAND; }
int dm_game_interact_context(void) { return DM_INTERACT_CTX_TALK; }
#endif
