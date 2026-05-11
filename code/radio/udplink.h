#pragma once

#include "../base/base.h"

void radio_process_udp_init();
void radio_process_udp_free();

u8* radio_process_udp_data_in(int* outPacketLength, u32 uTimeNow);
u32 radio_process_udp_data_out(u8* pBuffer, u32 pBufferLen);

int radio_process_udp_open_rx();
int radio_process_udp_open_tx();