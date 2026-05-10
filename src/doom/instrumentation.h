#ifndef __INSTRUMENTATION__
#define __INSTRUMENTATION__

typedef struct
{
    int damage_taken;
    int damage_dealt;
} ins_outbound_packet_t;

extern ins_outbound_packet_t ins_outbound;

void INS_Init(const char* host, int port);

void INS_BeginFrame();
void INS_EndTransmit();

#endif