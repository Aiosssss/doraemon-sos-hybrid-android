// Optional Atmosphere Cheat Engine + Built-in v1.1.2 Cheats + Live Tool Context Reader.
// All cheats default to OFF (master = 0). When master == 0, dm_cheats_tick() is a 1-instruction atomic return.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tool context enum matching assets/overlay/adaptive.png tiles 0..10 (Button Y)
enum {
    DM_TOOL_CTX_HAND = 0,
    DM_TOOL_CTX_WATERING_CAN = 1,
    DM_TOOL_CTX_HOE = 2,
    DM_TOOL_CTX_HAMMER = 3,
    DM_TOOL_CTX_AXE = 4,
    DM_TOOL_CTX_SICKLE = 5,
    DM_TOOL_CTX_FISHING_ROD = 6,
    DM_TOOL_CTX_BUG_NET = 7,
    DM_TOOL_CTX_SEEDS = 8,
    DM_TOOL_CTX_ANIMAL = 9,
    DM_TOOL_CTX_CARRY = 10,
};

// Interact context enum matching assets/overlay/adaptive.png tiles 11..13 (Button A)
enum {
    DM_INTERACT_CTX_TALK = 0,     // tile 11: speech bubble (active gameplay / NPC talk)
    DM_INTERACT_CTX_CONFIRM = 1,  // tile 12: checkmark (title screen / loading / menu)
    DM_INTERACT_CTX_HARVEST = 2,  // tile 13: harvest / give hand + heart (carrying item)
};

// Initialize / reload cheat state & imported Atmosphere .txt files from $DM_ROOT/cheats/
void dm_cheats_init(const char *root_dir);
void dm_cheats_reload(void);

// Run one frame tick of active cheats (safe to call from present/main thread; instant return when OFF)
void dm_cheats_tick(void);

// Master switch & per-cheat controls (for In-Game Pause Menu; auto-persists to cheats_state.tsv)
int dm_cheats_master(void);
void dm_cheats_set_master(int on);
int dm_cheats_count(void);
int dm_cheats_info(int idx, char *name_out, size_t nsz, int *enabled_out, int *is_builtin_out);
void dm_cheats_toggle(int idx);

// Live equipped tool & situation detectors for Adaptive Button Level 2 (100% read-only & vm_read_overwrite guarded)
int dm_game_tool_context(void);
int dm_game_interact_context(void);

// Held-slot safety guard evaluator for built-in cheat #12 (Item Tak Habis - Slot Dipegang)
int dm_cheat_eval_held_item_guard(int sec_type, int32_t max_stack, int32_t item_id, int32_t count,
                                  uint64_t tool_ptr, int32_t master_tool_id, uint8_t is_important, int tool_ctx);
#ifdef CHEAT_ENGINE_HOST_TEST
void dm_cheat_test_set_held_safe(int safe);
#endif

#ifdef __cplusplus
}
#endif
