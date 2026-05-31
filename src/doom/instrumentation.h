#ifndef __INSTRUMENTATION__
#define __INSTRUMENTATION__

#include "d_player.h"

void INS_Init(const char* host, int port);

void INS_BeginFrame();

void INS_InjectInput(ticcmd_t* cmd);

void INS_Process(const player_t* player);

void INS_UpdateScreen(const pixel_t* framebuffer);

void INS_EndTransmit();

#endif