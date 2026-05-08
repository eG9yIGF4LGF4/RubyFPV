#include "radiolink.h"
#include "udplink.h"

u8 sPacketBufferRead[MAX_PACKET_LENGTH_PCAP];
struct sockaddr_in server_out_addr, server_in_addr;

void radio_process_udp_init(int isController)
{
   memset(&server_in_addr, 0, sizeof(server_in_addr));
   memset(&server_out_addr, 0, sizeof(server_out_addr));
   memset(sPacketBufferRead, 0, MAX_PACKET_LENGTH_PCAP);
    
   server_in_addr.sin_family = AF_INET;
   server_in_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
   server_in_addr.sin_port = htons( isController == 1 ? UDP_PORT_STATION : UDP_PORT_VEHICLE );   

   server_out_addr.sin_family = AF_INET;
   server_out_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
   server_out_addr.sin_port = htons( isController == 1 ? UDP_PORT_VEHICLE : UDP_PORT_STATION );   
}

u8* radio_process_udp_data_in(int interfaceNumber, int* outPacketLength, u32 uTimeNow)
{
   u8* buf[MAX_PACKET_LENGTH_PCAP];
   int s = socket(AF_INET, SOCK_DGRAM, 0);
   
   *outPacketLength = recvfrom(s, buf, MAX_PACKET_LENGTH_PCAP, 0, (struct sockaddr *)&server_in_addr, sizeof(server_in_addr));

   memcpy(sPacketBufferRead, buf, *outPacketLength);
   
   return sPacketBufferRead;
}

u32 radio_process_udp_data_out(int interfaceNumber, u8* pBuffer, u32 pBufferLen)
{
   int s = socket(AF_INET, SOCK_DGRAM, 0); 

   return sendto(s, pBuffer, pBufferLen, 0, (struct sockaddr *)&server_out_addr, sizeof(server_out_addr) );
}

int radio_process_udp_open_rx(int isController)
{
   return 0;  
}

int radio_process_udp_open_tx(int isController)
{
   return 0;
}

