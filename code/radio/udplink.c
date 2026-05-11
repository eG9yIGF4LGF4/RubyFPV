#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include "radiolink.h"
#include "udplink.h"

// TODO: For a wg0 interface with 192.168.43.1/24 subnet
#define IP_TUNNEL_UDP_PORT      6662
#define IP_TUNNEL_ADDR_STATION  "192.168.43.2"
#define IP_TUNNEL_ADDR_VEHICLE  "192.168.43.3"

static u8  sPacketBufferRead[MAX_PACKET_LENGTH_PCAP];

static int s_rx_sock       = -1;
static int s_tx_sock       = -1;
static int s_is_controller = 0;

static struct sockaddr_in s_bind_addr;  /* local address  — RX binds here  */
static struct sockaddr_in s_dest_addr;  /* remote address — TX sends here  */

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void radio_process_udp_init(int isController)
{
    s_is_controller = isController;
    memset(sPacketBufferRead, 0, MAX_PACKET_LENGTH_PCAP);

    /*
     * Controller  : local = 192.168.43.1, remote = 192.168.43.2
     * Vehicle     : local = 192.168.43.2, remote = 192.168.43.1
     */
    const char *local_ip  = isController ? IP_TUNNEL_ADDR_STATION  : IP_TUNNEL_ADDR_VEHICLE;
    const char *remote_ip = isController ? IP_TUNNEL_ADDR_VEHICLE : IP_TUNNEL_ADDR_STATION;

    memset(&s_bind_addr, 0, sizeof(s_bind_addr));
    s_bind_addr.sin_family      = AF_INET;
    s_bind_addr.sin_addr.s_addr = inet_addr(local_ip);
    s_bind_addr.sin_port        = htons(IP_TUNNEL_UDP_PORT);

    memset(&s_dest_addr, 0, sizeof(s_dest_addr));
    s_dest_addr.sin_family      = AF_INET;
    s_dest_addr.sin_addr.s_addr = inet_addr(remote_ip);
    s_dest_addr.sin_port        = htons(IP_TUNNEL_UDP_PORT);
}

int radio_process_udp_open_rx(void)
{
    if (s_rx_sock >= 0)
        close(s_rx_sock);

    s_rx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_rx_sock < 0)
        return -1;

    int opt = 1;
    setsockopt(s_rx_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(s_rx_sock, (struct sockaddr *)&s_bind_addr, sizeof(s_bind_addr)) < 0)
    {
        close(s_rx_sock);
        s_rx_sock = -1;
        return -1;
    }

    return 0;
}

int radio_process_udp_open_tx(void)
{
    if (s_tx_sock >= 0)
        close(s_tx_sock);

    s_tx_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (s_tx_sock < 0)
        return -1;

    return 0;
}

u8* radio_process_udp_data_in(int interfaceNumber, int* outPacketLength, u32 uTimeNow)
{
    (void)interfaceNumber;
    (void)uTimeNow;

    *outPacketLength = 0;

    if (s_rx_sock < 0)
        return NULL;

    struct sockaddr_in src_addr;
    socklen_t src_len = sizeof(src_addr);

    int n = recvfrom(s_rx_sock,
                     sPacketBufferRead, MAX_PACKET_LENGTH_PCAP,
                     0,
                     (struct sockaddr *)&src_addr, &src_len);
    if (n < 0)
        return NULL;

    *outPacketLength = n;
    return sPacketBufferRead;
}

u32 radio_process_udp_data_out(int interfaceNumber, u8* pBuffer, u32 pBufferLen)
{
    (void)interfaceNumber;

    if (s_tx_sock < 0)
        return 0;

    int n = sendto(s_tx_sock,
                   pBuffer, pBufferLen,
                   0,
                   (struct sockaddr *)&s_dest_addr, sizeof(s_dest_addr));
    return (n < 0) ? 0 : (u32)n;
}