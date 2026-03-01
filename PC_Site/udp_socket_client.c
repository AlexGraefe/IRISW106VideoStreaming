#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <stdint.h>
#include <time.h>

#define PORT               8080
#define STREAM_FLOAT_COUNT 350
#define WARMUP_STEPS       100
#define RECORD_STEPS       100000
#define OUTPUT_CSV         "results.csv"

/* Must match the server definition exactly */
typedef struct __attribute__((packed)) {
    uint32_t counter;
    float    data[STREAM_FLOAT_COUNT];
} __attribute__((packed)) stream_packet_t;

#define PACKET_SIZE ((int)sizeof(stream_packet_t))

int main(int argc, char *argv[])
{
    /* Allocate recording arrays */
    ssize_t  rec_bytes[RECORD_STEPS];
    int32_t  rec_drops[RECORD_STEPS];
    double   rec_time_us[RECORD_STEPS];  /* receive duration in microseconds */

    static stream_packet_t pkt;   /* static: keeps large buffer off the stack */

    uint32_t last_counter = 0;
    int      step         = 0;   /* counts valid packets after warmup */
    int      warmup       = 0;   /* counts warmup packets */
    int      first_pkt    = 1;


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

    /* Send START trigger */
    const char *start_msg = "START";
    if (send(sock_fd, start_msg, strlen(start_msg), 0) < 0) {
        perror("send START");
        close(sock_fd);
        exit(EXIT_FAILURE);
    }
    // printf("[Client] Warming up for %d packets...\n", WARMUP_STEPS);

    struct timespec ts_before, ts_after;

    for (;;) {
        ts_before = ts_after;
        clock_gettime(CLOCK_MONOTONIC, &ts_after);
        ssize_t bytes = recv(sock_fd, &pkt, PACKET_SIZE, 0);
        if (bytes < 0) {
            perror("recv");
            close(sock_fd);
            exit(EXIT_FAILURE);
        }
        /* On the very first packet just seed last_counter */
        if (first_pkt) {
            last_counter = pkt.counter;
            first_pkt = 0;
            continue;
        }

        /* Warmup phase */
        if (warmup < WARMUP_STEPS) {
            last_counter = pkt.counter;
            warmup++;
            if (warmup == WARMUP_STEPS) {
                printf("[Client] Warmup done. Recording %d packets...\n", RECORD_STEPS);
            }
            continue;
        }



        /* Recording phase */
        int32_t dropped = (int32_t)(pkt.counter - last_counter) - 1;

        if (bytes != PACKET_SIZE) {
            dropped += 1;   /* count as drop if packet is incomplete */
            bytes = 0;
        }


        if (dropped < 0) {
            dropped = 0;   /* counter wrapped or resynced */
        }

        long long before_us = ts_before.tv_sec * 1000000LL + ts_before.tv_nsec / 1000;
        long long after_us  = ts_after.tv_sec  * 1000000LL + ts_after.tv_nsec  / 1000;
        double dt_us = (double)(after_us - before_us);

        rec_bytes[step]   = bytes;
        rec_drops[step]   = dropped;
        rec_time_us[step] = dt_us;
        step++;

        last_counter = pkt.counter;

        if (step >= RECORD_STEPS) {
            break;
        }
    }

    close(sock_fd);
    printf("[Client] Recording complete. Writing %s...\n", OUTPUT_CSV);

    /* Write CSV */
    FILE *fp = fopen(OUTPUT_CSV, "w");
    if (!fp) {
        perror("fopen");
        exit(EXIT_FAILURE);
    }

    fprintf(fp, "step,bytes,dropped_packets,recv_time_us\n");
    for (int i = 0; i < RECORD_STEPS; i++) {
        fprintf(fp, "%d,%zd,%d,%.3f\n", i, rec_bytes[i], rec_drops[i], rec_time_us[i]);
    }
    fclose(fp);

    printf("[Client] Saved to %s\n", OUTPUT_CSV);
    return 0;
}

