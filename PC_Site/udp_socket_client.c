#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>

#define PORT             8080
#define STREAM_FLOAT_COUNT 350

/* Must match the server definition exactly */
typedef struct __attribute__((packed)) {
    uint32_t counter;
    float    data[STREAM_FLOAT_COUNT];
} stream_packet_t;

#define PACKET_SIZE ((int)sizeof(stream_packet_t))

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <server-ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *server_ip = argv[1];
    int sock_fd;
    struct sockaddr_in server_addr;

    /* Create UDP socket */
    sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* Build server address */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons(PORT);

    if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    /* Connect so send/recv are implicitly addressed to the server */
    if (connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("connect");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }

    /* Send start command — this also reveals our port to the server */
    const char *start_msg = "START";
    if (send(sock_fd, start_msg, strlen(start_msg), 0) < 0) {
        perror("send START");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }
    printf("[Client] Sent start command to %s:%d — waiting for stream...\n",
           server_ip, PORT);

    /* Receive streaming packets and print the counter */
    static stream_packet_t pkt;   /* static: 16 KB off the stack */
    
    int32_t last_counter = 0;
    uint32_t dropped_packets = 0;

    struct timespec start_time, current_time;
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    uint64_t total_bytes = 0;

    for (;;) {
        ssize_t bytes = recv(sock_fd, &pkt, PACKET_SIZE, 0);
        if (bytes < 0) {
            perror("recv");
            break;
        }
        if (bytes != PACKET_SIZE) {
            fprintf(stderr, "[Client] Warning: unexpected packet size %zd (expected %d)\n",
                    bytes, PACKET_SIZE);
            continue;
        }
        total_bytes += bytes;

        if (last_counter + 1 != pkt.counter) {
            dropped_packets += pkt.counter - last_counter - 1;
        }
        clock_gettime(CLOCK_MONOTONIC, &current_time);
        double elapsed = (current_time.tv_sec - start_time.tv_sec) + 
                        (current_time.tv_nsec - start_time.tv_nsec) / 1e9;
        if (elapsed > 0) {
            double datarate_mbps = (total_bytes) / (elapsed * 1e6);
            printf("%.2f MBps %f\n", 
                    datarate_mbps,  
                    100.0f * dropped_packets / (pkt.counter - 1.0f));
        }

        last_counter = pkt.counter;
    }

    close(sock_fd);
    printf("[Client] Closed.\n");
    return 0;
}

