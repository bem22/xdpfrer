#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdint.h>

#define PROGRESS_INTERVAL 1000

struct payload {
    uint32_t seq;
    int64_t tv_sec;
    int64_t tv_nsec;
};

static void build_filename(char *out, size_t out_sz, const char *base)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(out, out_sz, "%s_%04d%02d%02d_%02d%02d%02d.csv",
             base,
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [-i IP] [-p PORT] [-c COUNT] [-s SIZE] [-d DELAY_US] [-o NAME]\n", prog);
    printf("  -i : Target IP (Default: 10.0.0.2)\n");
    printf("  -p : Target Port (Default: 9999)\n");
    printf("  -c : Packet Count (Default: 100)\n");
    printf("  -s : Packet Size in bytes (Default: 64, Min: %lu)\n", sizeof(struct payload));
    printf("  -d : Delay between packets in microseconds (Default: 10000 = 10ms)\n");
    printf("  -o : Output CSV base name (Default: sender)\n");
}

int main(int argc, char *argv[])
{
    int sockfd;
    struct sockaddr_in server_addr;
    struct timespec send_time, sleep_time;

    char target_ip[64] = "10.0.0.2";
    int port = 9999;
    uint32_t count = 100;
    int size = 64;
    int delay_us = 10000;
    char csv_base[256] = "sender";

    int opt;
    while ((opt = getopt(argc, argv, "i:p:c:s:d:o:h")) != -1) {
        switch (opt) {
            case 'i': strncpy(target_ip, optarg, sizeof(target_ip) - 1); break;
            case 'p': port = atoi(optarg); break;
            case 'c': count = (uint32_t)atoi(optarg); break;
            case 's': size = atoi(optarg); break;
            case 'd': delay_us = atoi(optarg); break;
            case 'o': strncpy(csv_base, optarg, sizeof(csv_base) - 1); break;
            case 'h': print_usage(argv[0]); exit(0);
            default:  print_usage(argv[0]); exit(EXIT_FAILURE);
        }
    }

    if ((size_t)size < sizeof(struct payload))
        size = sizeof(struct payload);

    char *buffer = calloc(1, size);
    if (!buffer) { perror("calloc"); exit(EXIT_FAILURE); }

    /* Build output CSV filename */
    char csv_path[512];
    build_filename(csv_path, sizeof(csv_path), csv_base);
    FILE *csv = fopen(csv_path, "w");
    if (!csv) { perror("fopen csv"); exit(EXIT_FAILURE); }
    fprintf(csv, "seq,send_epoch_ns\n");

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("Socket creation failed");
        exit(EXIT_FAILURE);
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    inet_pton(AF_INET, target_ip, &server_addr.sin_addr);

    sleep_time.tv_sec = delay_us / 1000000;
    sleep_time.tv_nsec = (delay_us % 1000000) * 1000L;

    printf("--- FRER Sender Started ---\n");
    printf("Target: %s:%d | Count: %u | Size: %d bytes | Interval: %d us\n",
           target_ip, port, count, size, delay_us);
    printf("CSV  : %s\n", csv_path);

    for (uint32_t i = 1; i <= count; i++) {
        clock_gettime(CLOCK_REALTIME, &send_time);

        struct payload *pkt = (struct payload *)buffer;
        pkt->seq = i;
        pkt->tv_sec  = (int64_t)send_time.tv_sec;
        pkt->tv_nsec = (int64_t)send_time.tv_nsec;

        sendto(sockfd, buffer, size, 0,
               (const struct sockaddr *)&server_addr, sizeof(server_addr));

        int64_t send_ns = (int64_t)send_time.tv_sec * 1000000000LL + send_time.tv_nsec;
        fprintf(csv, "%u,%lld\n", i, (long long)send_ns);

        if (i % PROGRESS_INTERVAL == 0)
            printf("  sent %u / %u\n", i, count);

        nanosleep(&sleep_time, NULL);
    }

    fclose(csv);
    printf("Transmission complete. CSV written to %s\n", csv_path);
    close(sockfd);
    free(buffer);
    return 0;
}