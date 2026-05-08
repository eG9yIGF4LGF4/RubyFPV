#pragma once

#include "../base/base.h"

#define UDP_PORT_STATION 6620
#define UDP_PORT_VEHICLE 6621

void radio_process_udp_init(int isController);

u8* radio_process_udp_data_in(int interfaceNumber, int* outPacketLength, u32 uTimeNow);
u32 radio_process_udp_data_out(int interfaceNumber, u8* pBuffer, u32 pBufferLen);

int radio_process_udp_open_rx(s);
int radio_process_udp_open_tx();