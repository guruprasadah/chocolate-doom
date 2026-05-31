#ifndef __INS_TYPES__
#define __INS_TYPES__

#include <stdint.h>
#include <stdbool.h>

#pragma pack(push, 1)
typedef struct
{
    uint8_t episode_ended;
    uint8_t dead;

    uint32_t damage_taken;
    uint32_t damage_dealt;
    uint32_t kill_count;
    uint32_t ammo_used;

    uint32_t interactions;
    
    uint32_t ammo_picked_up;
    uint32_t weapons_picked_up;
    uint32_t health_picked_up;
    uint32_t armor_picked_up;
    uint32_t cards_picked_up;

    float x, y;

    float velocity;

    float angle_to_enemy;
    float dist_to_enemy;

    float framebuffer[320 * 200];
} ins_outbound_state_t;
#pragma pack(pop)

extern ins_outbound_state_t ins_outbound;

#pragma pack(push, 1)
typedef struct
{
    int8_t forwardmove;
    int8_t sidemove;
    int8_t turn;
    uint8_t attack;
    uint8_t use;

    uint8_t restart;
} ins_inbound_input_t;
#pragma pack(pop)

extern ins_inbound_input_t ins_input;

extern bool ins_net_init;

#endif