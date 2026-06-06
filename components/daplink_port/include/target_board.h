#pragma once

typedef struct {
    const char *target_vendor;
    const char *target_part_number;
} target_cfg_t;

typedef struct {
    const target_cfg_t *target_cfg;
    const char *board_vendor;
    const char *board_name;
} board_info_t;

extern const board_info_t g_board_info;
