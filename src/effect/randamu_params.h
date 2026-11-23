// randamu: parameter indices. The index is also the AEGP effect stream index
// (AEGP_GetNewEffectStreamByIndex), so the order must never change once shipped.
#pragma once

enum RandamuParam {
    RP_INPUT = 0,
    RP_STATUS,        // read-only checkbox whose label shows the hook / 3D status
    RP_COUNT,
    RP_SEED,
    RP_POS_X,
    RP_POS_Y,
    RP_POS_Z,
    RP_ROT_X,
    RP_ROT_Y,
    RP_ROT_Z,
    RP_SCALE_MIN,
    RP_SCALE_MAX,
    RP_OPACITY_MIN,
    RP_OPACITY_MAX,
    RP_KEEP_FIRST,
    RP_NUM_PARAMS
};

// Disk IDs (persisted in projects). Never reuse or renumber.
enum RandamuDiskId {
    RD_STATUS = 1,
    RD_COUNT,
    RD_SEED,
    RD_POS_X,
    RD_POS_Y,
    RD_POS_Z,
    RD_ROT_X,
    RD_ROT_Y,
    RD_ROT_Z,
    RD_SCALE_MIN,
    RD_SCALE_MAX,
    RD_OPACITY_MIN,
    RD_OPACITY_MAX,
    RD_KEEP_FIRST,
};

#define RANDAMU_MATCH_NAME "RANDAMU Scatter"
