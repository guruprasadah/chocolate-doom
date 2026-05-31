#include "doom/instrumentation.h"
#include "d_main.h"
#include "d_player.h"
#include "doom/d_items.h"
#include "doom/doomstat.h"
#include "doomkeys.h"
#include "g_game.h"
#include "doom/doomdef.h"
#include "p_local.h"
#include "d_ticcmd.h"
#include "instrumentation_types.h"

#include "d_event.h"
#include "doomtype.h"
#include "m_fixed.h"
#include "w_wad.h"
#include "z_zone.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <errno.h>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int ins_socket = -1;
bool ins_net_init = false;

static int last_ammo_count = -1;
static int last_weapon = -1;

ins_outbound_state_t ins_outbound;

ins_inbound_input_t ins_input;

byte grayscale_lookup[256];

void INS_Init(const char *host, int port)
{
    byte *doom_palette;
    int i;

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    printf("[INS] Initializing network connection to %s:%d\n", host, port);

    ins_socket = socket(AF_INET, SOCK_STREAM, 0);
    if(ins_socket < 0)
    {
        printf("[INS] Failed to create socket, exiting\n");
    }

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &server_addr.sin_addr) <= 0) {
        printf("[INS] Invalid address\n");
        close(ins_socket);
        return;
    }

    if (connect(ins_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        printf("[INS] Connection failed (data server not running?)\n");
        close(ins_socket);
        ins_socket = -1;
        return;
    }
    
    doom_palette = W_CacheLumpName("PLAYPAL", PU_STATIC);
    
    // Build lookup table
    for (i = 0; i < 256; i++)
    {
        byte r = doom_palette[i * 3 + 0];
        byte g = doom_palette[i * 3 + 1];
        byte b = doom_palette[i * 3 + 2];
        
        int gray = (int)(0.299f * r + 0.587f * g + 0.114f * b);
        grayscale_lookup[i] = (byte)(gray > 255 ? 255 : gray);
    }
    
    W_ReleaseLumpName("PLAYPAL");

    ins_net_init = true;
    printf("[INS] Init successful, connected to data server\n");
}

ssize_t send_all(int sock, const void *buf, size_t len)
{
    size_t total = 0;
    const char *p = buf;

    while (total < len) {
        ssize_t n = send(sock, p + total, len - total, 0);

        if (n < 0) {
            return -1;
        }

        total += n;
    }

    return total;
}

ssize_t recv_all(int sock, void *buf, size_t len)
{
    size_t total = 0;
    char *p = buf;

    while (total < len)
    {
        ssize_t n = recv(sock, p + total, len - total, 0);

        if (n <= 0)
        {
            return -1;
        }

        total += n;
    }

    return total;
}

bool check_sock_alive()
{
    char buf;
    int ret = recv(ins_socket, &buf, 1, MSG_PEEK | MSG_DONTWAIT);

    if (ret > 0) {
        return true;
    }
    else if (ret == 0) {
        return false;
    }
    else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return true;
        } else {
            return false;
        }
    }
}

bool INS_WaitForInput()
{
    if (!ins_net_init)
        return false;

    if (recv_all(ins_socket, &ins_input,
                 sizeof(ins_inbound_input_t)) < 0)
    {
        fprintf(stderr, "[INS] Failed receiving input packet\n");
        ins_net_init = false;
        return false;
    }

    return true;
}

void INS_BeginFrame()
{
    memset(&ins_outbound, 0, sizeof(ins_outbound_state_t));
    if(ins_input.restart) {
        gameaction = ga_loadlevel;
    }
}

void INS_InjectInput(ticcmd_t *cmd) {
    if(!ins_net_init)
        return;

    cmd->forwardmove += ins_input.forwardmove;
    cmd->sidemove += ins_input.sidemove;
    
    

    if(ins_input.attack)
    {
        cmd->buttons |= BT_ATTACK;
    }
    if(ins_input.use)
    {
        cmd->buttons |= BT_USE;
    }
}

// Helper: Convert angle difference to -180 to +180 range
static float NormalizeAngleDifference(angle_t angle_diff)
{
    // angle_t is 32-bit: 0x80000000 = 180°
    // Convert to signed representation
    
    int signed_angle = (int)angle_diff;
    
    // Convert BAM angle to degrees
    // BAM: 0x00000000 = 0°, 0x40000000 = 90°, 0x80000000 = 180°
    // So: degrees = (angle * 360) / 0x100000000
    float degrees = (float)signed_angle * 360.0f / 4294967296.0f;
    
    // Normalize to -180 to +180
    while (degrees > 180.0f)
        degrees -= 360.0f;
    while (degrees < -180.0f)
        degrees += 360.0f;
    
    return degrees;
}

// Helper: Check if enemy is visible to player
// Returns true if enemy has line of sight and is within viewable angle
static boolean IsEnemyOnScreen(mobj_t *player_mo, mobj_t *enemy)
{
    if (!player_mo || !enemy)
        return false;
    
    // Check 1: Line of sight (unobstructed view)
    if (!P_CheckSight(player_mo, enemy))
        return false;
    
    // Check 2: Within player's field of view (~170 degrees forward)
    angle_t angle_to_enemy = R_PointToAngle2(player_mo->x, player_mo->y,
                                             enemy->x, enemy->y);
    angle_t angle_diff = angle_to_enemy - player_mo->angle;
    
    // If angle difference is greater than ~85 degrees (0x15555555 BAM units)
    // in either direction, it's outside the typical FOV cone
    // 85 degrees ≈ 0x15555555 in BAM
    const angle_t FOV_HALF = 0x15555555;
    
    // Normalize to shortest angular distance
    if (angle_diff > 0x80000000)
        angle_diff = 0xffffffff - angle_diff + 1;
    
    if (angle_diff > FOV_HALF)
        return false;  // Outside FOV
    
    return true;  // Enemy is visible and on-screen
}

float INS_GetAngleToNearestEnemy(player_t *player)
{
    mobj_t *mo = player->mo;
    mobj_t *thing;
    fixed_t nearest_dist = INT_MAX;
    mobj_t *nearest_enemy = NULL;
    
    if (!mo)
        return 180.0f;  // No player, return "no enemy found" angle
    
    // Iterate through all thinkers to find shootable enemies
    extern thinker_t thinkercap;
    thinker_t *th;
    
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        
        thing = (mobj_t *)th;
        
        // Skip: player, non-shootable things, dead things, friendly things
        if (thing == mo)
            continue;
        if (!(thing->flags & MF_SHOOTABLE))
            continue;
        if (thing->health <= 0)
            continue;
        if (thing->player)  // Other players (in multiplayer)
            continue;
        
        // CHECK: Is enemy actually visible on screen?
        if (!IsEnemyOnScreen(mo, thing))
            continue;
        
        // Calculate distance
        fixed_t dx = thing->x - mo->x;
        fixed_t dy = thing->y - mo->y;
        fixed_t dist = FixedMul(dx, dx) + FixedMul(dy, dy);
        
        // Track nearest
        if (dist < nearest_dist)
        {
            nearest_dist = dist;
            nearest_enemy = thing;
        }
    }
    
    // No visible enemy found
    if (!nearest_enemy)
        return 180.0f;  // Return 180° (behind player)
    
    // Calculate angle from player to enemy
    angle_t angle_to_enemy = R_PointToAngle2(mo->x, mo->y, 
                                             nearest_enemy->x, nearest_enemy->y);
    
    // Calculate difference from player's current heading
    angle_t angle_diff = angle_to_enemy - mo->angle;
    
    // Convert to -180 to +180 degrees
    return NormalizeAngleDifference(angle_diff);
}

fixed_t INS_GetDistanceToNearestEnemy(player_t *player)
{
    mobj_t *mo = player->mo;
    mobj_t *thing;
    fixed_t nearest_dist = INT_MAX;
    
    if (!mo)
        return 0;
    
    extern thinker_t thinkercap;
    thinker_t *th;
    
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        
        thing = (mobj_t *)th;
        
        if (thing == mo)
            continue;
        if (!(thing->flags & MF_SHOOTABLE))
            continue;
        if (thing->health <= 0)
            continue;
        if (thing->player)
            continue;
        
        // CHECK: Is enemy visible on screen?
        if (!IsEnemyOnScreen(mo, thing))
            continue;
        
        fixed_t dx = thing->x - mo->x;
        fixed_t dy = thing->y - mo->y;
        
        fixed_t dist = P_AproxDistance(dx, dy);
        
        if (dist < nearest_dist)
            nearest_dist = dist;
    }
    
    return (nearest_dist == INT_MAX) ? 0 : nearest_dist;
}

void INS_Process(const player_t *player)
{
    mobj_t* mo;
    fixed_t x = 0, y = 0, speed = 0;

    if(player->mo == NULL)
        return;

    mo = player->mo;
    x = mo->momx;
    y = mo->momy;
    speed = P_AproxDistance(x, y);
    ins_outbound.velocity = (float)speed / FRACUNIT;

    ins_outbound.x = (float)x / FRACUNIT;
    ins_outbound.y = (float)y / FRACUNIT;

    if(player->playerstate == PST_DEAD)
    {
        ins_outbound.dead = 1;
    }

    ammotype_t ammoIndex = weaponinfo[player->readyweapon].ammo;

        // Ensure the current weapon actually uses ammo (e.g., not Chainsaw/Fists)
        if (ammoIndex != am_noammo) 
        {
            int current_ammo = player->ammo[ammoIndex];

            // If the weapon changed, reset the baseline to avoid false deltas
            if (player->readyweapon != last_weapon) 
            {
                last_ammo_count = current_ammo;
                last_weapon = player->readyweapon;
            }

            // Calculate delta if we have a valid baseline
            if (last_ammo_count != -1) 
            {
                int delta = last_ammo_count - current_ammo;

                if (delta > 0) 
                {
                    // Delta found! Log it, send it to an API, or export it
                    ins_outbound.ammo_used = delta;
                }
            }

            // Update the baseline for the next frame/tic
            last_ammo_count = current_ammo;
        }
        else 
        {
            // Reset trackers if player switches to a melee weapon
            last_ammo_count = -1;
            last_weapon = -1;
        }

        ins_outbound.angle_to_enemy = INS_GetAngleToNearestEnemy(&players[consoleplayer]);
        ins_outbound.dist_to_enemy = (float)INS_GetDistanceToNearestEnemy(&players[consoleplayer]) / 65536.0;
}

void INS_Util_ExtractGrayscaleFrame(const pixel_t* input, float* output)
{
    int i;   
    // Convert to float [0, 1] and output
    for (i = 0; i < SCREENWIDTH * SCREENHEIGHT; i++)
    {
        byte gray = grayscale_lookup[input[i]];
        output[i] = (float)gray / 255.0f;  // Normalize to [0, 1]
    }
}

void INS_UpdateScreen(const pixel_t* framebuffer)
{
    INS_Util_ExtractGrayscaleFrame(framebuffer, ins_outbound.framebuffer);
}

void INS_EndTransmit()
{
    if(ins_net_init && !check_sock_alive())
    {
        ins_net_init = false;
        fprintf(stderr, "[INS] Socket has become invalid\n");
        return;
    }
    if(!ins_net_init)
        return;
    if(gameaction == ga_completed)
    {
        ins_outbound.episode_ended = 1;
    }
    if(send_all(ins_socket, &ins_outbound, sizeof(ins_outbound_state_t)) < 0)
    {
        fprintf(stderr, "[INS] Failed to send data to server\n");
    }

    if(!INS_WaitForInput())
    {
        fprintf(stderr, "[INS] Failed receiving next input\n");
    }
}