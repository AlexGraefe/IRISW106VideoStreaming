#ifndef TCP_SOCKET_H
#define TCP_SOCKET_H

#define SERVER_PORT      8080
#define BUFFER_SIZE      256
#define STREAM_FLOAT_COUNT 350

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/net/socket.h>

#define SOCKET_THREAD_PRIORITY 10

#define LED0_NODE DT_ALIAS(led0)
#define LED1_NODE DT_ALIAS(led1)
#define LED2_NODE DT_ALIAS(led2)

#define SPI_NODE DT_ALIAS(spi)

/* Packet streamed from server to client */
typedef struct {
	uint32_t counter;
	float    data[STREAM_FLOAT_COUNT];
} __attribute__((packed)) stream_packet_t;

typedef enum {
	COMM_WIFI_CONNECTING,
	COMM_WAITING_FOR_IP,
	COMM_ESTABLISHING_SERVER,
	COMM_SENDING_MESSAGES,
	COMM_FAILURE,
	COMM_CLEANUP,
	COMM_DONE,
} communication_state_t;

typedef struct {
	struct sockaddr_in server_addr;
	struct sockaddr    client_addr;
	net_socklen_t      client_addr_len;
	stream_packet_t    stream_pkt;
	char               buffer[BUFFER_SIZE];
	char               ip_addr[NET_IPV4_ADDR_LEN];
	char               client_ip_addr[NET_IPV4_ADDR_LEN];
	int  sock_fd;
	bool wifi_connected;
	bool socket_open;
	int  exit_code;
	communication_state_t failure_from_state;
} communication_context_t;

int run_udp_socket_demo(void);

#endif /* TCP_SOCKET_H */
