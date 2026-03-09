#include <arpa/inet.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#include <unistd.h>

#define PORT 8080
#define IRIS_PACKET_PAYLOAD_SIZE 1400U

/* Must match the client definition exactly */
typedef struct __attribute__((packed)) {
	uint32_t frame_nmbr;
	uint32_t packet_idx;
	uint32_t packet_nmbr;
	uint8_t payload[IRIS_PACKET_PAYLOAD_SIZE];
} __attribute__((packed)) stream_packet_t;

#define PACKET_SIZE ((int)sizeof(stream_packet_t))

static volatile sig_atomic_t keep_running = 1;

static void handle_sigint(int signum)
{
	(void)signum;
	keep_running = 0;
}

static void fill_payload(stream_packet_t *pkt, uint32_t sequence)
{
	for (uint32_t i = 0; i < IRIS_PACKET_PAYLOAD_SIZE; ++i) {
		pkt->payload[i] = (uint8_t)((sequence + i) & 0xFFu);
	}
}

static int get_wifi_ipv4(char *out_ip, size_t out_ip_len)
{
	struct ifaddrs *ifaddr = NULL;
	if (getifaddrs(&ifaddr) == -1) {
		perror("getifaddrs");
		return -1;
	}

	int found = 0;
	for (struct ifaddrs *ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL) {
			continue;
		}
		if (ifa->ifa_addr->sa_family != AF_INET) {
			continue;
		}
		if ((ifa->ifa_flags & IFF_LOOPBACK) != 0) {
			continue;
		}
		if ((ifa->ifa_flags & IFF_UP) == 0) {
			continue;
		}
		if ((ifa->ifa_flags & IFF_RUNNING) == 0) {
			continue;
		}

		if (strncmp(ifa->ifa_name, "wl", 2) != 0) {
			continue;
		}

		struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
		if (inet_ntop(AF_INET, &addr->sin_addr, out_ip, out_ip_len) == NULL) {
			continue;
		}

		found = 1;
		break;
	}

	freeifaddrs(ifaddr);

	if (!found) {
		fprintf(stderr, "No active Wi-Fi IPv4 address found (interface name starting with 'wl').\n");
		return -1;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	long packet_limit = -1;    /* -1 => stream forever */
	long interval_us = 0;      /* 0 => as fast as possible */
	char bind_ip[INET_ADDRSTRLEN] = {0};

	if (argc >= 2) {
		packet_limit = strtol(argv[1], NULL, 10);
	}
	if (argc >= 3) {
		interval_us = strtol(argv[2], NULL, 10);
	}

	if (argc > 3) {
		fprintf(stderr, "Usage: %s [packet-count|-1] [interval-us]\n", argv[0]);
		return EXIT_FAILURE;
	}

	if (packet_limit < -1 || interval_us < 0) {
		fprintf(stderr, "Invalid arguments: packet-count must be >= -1 and interval-us >= 0\n");
		return EXIT_FAILURE;
	}

	if (get_wifi_ipv4(bind_ip, sizeof(bind_ip)) != 0) {
		return EXIT_FAILURE;
	}

	signal(SIGINT, handle_sigint);

	int sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock_fd < 0) {
		perror("socket");
		return EXIT_FAILURE;
	}

	struct sockaddr_in server_addr;
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(PORT);

	if (inet_pton(AF_INET, bind_ip, &server_addr.sin_addr) <= 0) {
		perror("inet_pton(bind-ip)");
		close(sock_fd);
		return EXIT_FAILURE;
	}

	if (bind(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
		perror("bind");
		close(sock_fd);
		return EXIT_FAILURE;
	}

	printf("[Server] Listening on Wi-Fi %s:%d\n", bind_ip, PORT);
	printf("[Server] Waiting for START...\n");

	char cmd_buf[64];
	struct sockaddr_in client_addr;
	socklen_t client_addr_len = sizeof(client_addr);
	ssize_t cmd_len = recvfrom(
		sock_fd,
		cmd_buf,
		sizeof(cmd_buf) - 1,
		0,
		(struct sockaddr *)&client_addr,
		&client_addr_len);

	if (cmd_len < 0) {
		perror("recvfrom");
		close(sock_fd);
		return EXIT_FAILURE;
	}

	cmd_buf[cmd_len] = '\0';
	if (strncmp(cmd_buf, "START", 5) != 0) {
		fprintf(stderr, "[Server] Unexpected trigger '%s'\n", cmd_buf);
		close(sock_fd);
		return EXIT_FAILURE;
	}

	char client_ip[INET_ADDRSTRLEN] = {0};
	inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));	
	printf("[Server] START from %s:%u\n", client_ip, ntohs(client_addr.sin_port));
	printf("[Server] Streaming packets (%s, interval=%ld us). Press Ctrl+C to stop.\n",
	       (packet_limit < 0 ? "unlimited" : "limited"), interval_us);

	stream_packet_t pkt;
	uint32_t seq = 0;
	long sent_count = 0;

	while (keep_running) {
		if (packet_limit >= 0 && sent_count >= packet_limit) {
			break;
		}

		pkt.frame_nmbr = htonl(seq / 10U);
		pkt.packet_idx = htonl(seq % 10U);
		pkt.packet_nmbr = htonl(seq);
		fill_payload(&pkt, seq);

		ssize_t sent = sendto(sock_fd,
		                     &pkt,
		                     PACKET_SIZE,
		                     0,
		                     (struct sockaddr *)&client_addr,
		                     client_addr_len);
		if (sent < 0) {
			if (errno == EINTR && !keep_running) {
				break;
			}
			perror("sendto");
			break;
		}

		if (sent != PACKET_SIZE) {
			fprintf(stderr, "[Server] Partial UDP send: %zd/%d\n", sent, PACKET_SIZE);
			break;
		}

		++seq;
		++sent_count;

		if ((sent_count % 500) == 0) {
			printf("[Server] Sent %ld packets\n", sent_count);
		}

		if (interval_us > 0) {
			usleep((useconds_t)interval_us);
		}
	}

	printf("[Server] Stopped after sending %ld packets\n", sent_count);
	close(sock_fd);
	return EXIT_SUCCESS;
}
