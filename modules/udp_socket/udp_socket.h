#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

#define SERVER_PORT      8080
#define BUFFER_SIZE      256

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/net/socket.h>

#define UDP_DATA_MAX_SIZE 60000

#define SOCKET_THREAD_PRIORITY 100

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)

#define SPI_NODE DT_ALIAS(spi)

#define IRIS_PACKET_PAYLOAD_SIZE 1400U
#define SPI_MAX_FRAME_SIZE       60000U

typedef struct __attribute__((packed))
{
	uint32_t frame_nmbr;
	uint32_t packet_idx;
	uint32_t packet_nmbr;
    uint8_t payload[IRIS_PACKET_PAYLOAD_SIZE];
} iris_packet_t;

typedef enum {
	COMM_WIFI_CONNECTING,
	COMM_WAITING_FOR_IP,
	COMM_ESTABLISHING_SERVER,
	COMM_CONNECTING_TO_CLIENT,
	COMM_SENDING_MESSAGES,
	COMM_FAILURE,
	COMM_CLEANUP,
	COMM_DONE,
} communication_state_t;

typedef struct {
	struct sockaddr_in server_addr;
	struct sockaddr    client_addr;
	net_socklen_t      client_addr_len;
	char               buffer[BUFFER_SIZE];
	char               ip_addr[NET_IPV4_ADDR_LEN];
	char               client_ip_addr[NET_IPV4_ADDR_LEN];
	int  sock_fd;
	bool wifi_connected;
	bool socket_open;
	int  exit_code;
	communication_state_t failure_from_state;
} communication_context_t;

typedef struct {
	uint8_t enabled;
} udp_message_t;

typedef struct {
	void *fifo_reserved;  // 1st word reserved for use by FIFO
	iris_packet_t packet;
} udp_data_t;

int run_udp_socket_demo(void);
struct k_fifo *get_udp_fifo(void);
struct k_msgq *get_udp_msgq(void);

#endif /* TCP_SOCKET_H */
