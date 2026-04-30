#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <time.h>
#include <stdint.h>
#include <signal.h>
#include <sys/socket.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>

#define ABS(x) ((x) < 0 ? -(x) : (x))

struct payload {
    uint32_t seq;
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* ── globals for summary report ── */
volatile sig_atomic_t keep_running = 1;
uint32_t received_count = 0;
uint32_t expected_count = 0;
uint32_t highest_seq = 0;
int64_t  min_lat_ns = INT64_MAX;
int64_t  max_lat_ns = INT64_MIN;
int64_t  sum_lat_ns = 0;
int64_t  prev_lat_ns = -1;
int64_t  jitter_sum_ns = 0;

static FILE *csv;

static void handle_sigint(int sig)
{
    (void)sig;
    keep_running = 0;
}

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

static void print_report(void)
{
    printf("\n\n--- FRER Tunnel Test Report ---\n");
    if (received_count == 0) {
        printf("No packets received.\n");
        return;
    }

    uint32_t lost = highest_seq > received_count ? highest_seq - received_count : 0;
    double loss_pct = highest_seq > 0 ? (double)lost / highest_seq * 100.0 : 0.0;
    double avg_lat  = (double)sum_lat_ns / received_count / 1e6;
    double avg_jit  = received_count > 1
                    ? (double)jitter_sum_ns / (received_count - 1) / 1e6
                    : 0.0;

    printf("Packets Sent (Est): %u\n", highest_seq);
    printf("Packets Received  : %u\n", received_count);
    printf("Packets Lost      : %u (%.2f%%)\n", lost, loss_pct);
    printf("-------------------------------\n");
    printf("Min Latency       : %.3f ms\n", (double)min_lat_ns / 1e6);
    printf("Max Latency       : %.3f ms\n", (double)max_lat_ns / 1e6);
    printf("Avg Latency       : %.3f ms\n", avg_lat);
    printf("Avg Jitter        : %.3f ms\n", avg_jit);
    printf("-------------------------------\n");
}

/* Try to extract the kernel SW RX timestamp from cmsg ancillary data.
 * Returns 1 on success (ts filled), 0 if not available.                */
static int extract_kernel_ts(struct msghdr *msg, struct timespec *ts)
{
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR(msg, cmsg))
    {
        if (cmsg->cmsg_level == SOL_SOCKET &&
            cmsg->cmsg_type  == SO_TIMESTAMPING)
        {
            /* struct scm_timestamping contains 3 timespecs:
             *  [0] software    [1] deprecated   [2] hardware     */
            struct timespec *stamps = (struct timespec *)CMSG_DATA(cmsg);
            if (stamps[0].tv_sec != 0 || stamps[0].tv_nsec != 0) {
                *ts = stamps[0];
                return 1;
            }
        }
    }
    return 0;
}

static void print_usage(const char *prog)
{
    printf("Usage: %s [-p PORT] [-c COUNT] [-o NAME]\n", prog);
    printf("  -p : Listen port (Default: 9999)\n");
    printf("  -c : Expected packet count, auto-stop (Default: 0 = unlimited)\n");
    printf("  -o : Output CSV base name (Default: receiver)\n");
}

int main(int argc, char *argv[])
{
    int sockfd;
    struct sockaddr_in server_addr;
    int port = 9999;
    char csv_base[256] = "receiver";

    int opt;
    while ((opt = getopt(argc, argv, "p:c:o:h")) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'c': expected_count = (uint32_t)atoi(optarg); break;
            case 'o': strncpy(csv_base, optarg, sizeof(csv_base) - 1); break;
            case 'h': print_usage(argv[0]); exit(0);
            default:  exit(EXIT_FAILURE);
        }
    }

    signal(SIGINT, handle_sigint);

    if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    /* ── Enable kernel software RX timestamps ── */
    int ts_flags = SOF_TIMESTAMPING_SOFTWARE | SOF_TIMESTAMPING_RX_SOFTWARE;
    if (setsockopt(sockfd, SOL_SOCKET, SO_TIMESTAMPING,
                   &ts_flags, sizeof(ts_flags)) < 0) {
        perror("SO_TIMESTAMPING (falling back to userspace timestamps)");
        /* non-fatal — we fall back to clock_gettime */
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family      = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port        = htons(port);

    if (bind(sockfd, (const struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }

    /* recv timeout so we can check keep_running */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* ── Open CSV ── */
    char csv_path[512];
    build_filename(csv_path, sizeof(csv_path), csv_base);
    csv = fopen(csv_path, "w");
    if (!csv) { perror("fopen csv"); exit(EXIT_FAILURE); }
    fprintf(csv, "seq,send_epoch_ns,recv_epoch_ns,latency_ns\n");

    printf("--- FRER Receiver Started ---\n");
    printf("Port : %d\n", port);
    printf("CSV  : %s\n", csv_path);
    printf("Press Ctrl+C to stop and generate report.\n\n");

    /* ── recvmsg buffers ── */
    char pkt_buf[65535];
    char ctrl_buf[256]; /* ancillary data for SO_TIMESTAMPING */
    struct iovec iov = { .iov_base = pkt_buf, .iov_len = sizeof(pkt_buf) };
    struct msghdr msg = {
        .msg_iov        = &iov,
        .msg_iovlen     = 1,
        .msg_control    = ctrl_buf,
        .msg_controllen = sizeof(ctrl_buf),
    };

    while (keep_running) {
        msg.msg_controllen = sizeof(ctrl_buf); /* reset each iteration */

        ssize_t n = recvmsg(sockfd, &msg, 0);
        if (n < 0)
            continue; /* timeout */

        /* ── Determine receive timestamp ── */
        struct timespec recv_ts;
        if (!extract_kernel_ts(&msg, &recv_ts))
            clock_gettime(CLOCK_REALTIME, &recv_ts); /* fallback */

        if ((size_t)n < sizeof(struct payload))
            continue;

        struct payload *pkt = (struct payload *)pkt_buf;
        received_count++;

        if (pkt->seq > highest_seq)
            highest_seq = pkt->seq;

        int64_t send_ns = pkt->tv_sec * 1000000000LL + pkt->tv_nsec;
        int64_t recv_ns = (int64_t)recv_ts.tv_sec * 1000000000LL + recv_ts.tv_nsec;
        int64_t lat_ns  = recv_ns - send_ns;

        /* CSV row */
        fprintf(csv, "%u,%lld,%lld,%lld\n",
                pkt->seq, (long long)send_ns, (long long)recv_ns, (long long)lat_ns);

        /* running stats */
        if (lat_ns < min_lat_ns) min_lat_ns = lat_ns;
        if (lat_ns > max_lat_ns) max_lat_ns = lat_ns;
        sum_lat_ns += lat_ns;

        if (prev_lat_ns >= 0)
            jitter_sum_ns += ABS(lat_ns - prev_lat_ns);
        prev_lat_ns = lat_ns;

        if (expected_count > 0 && received_count >= expected_count)
            break;
    }

    fclose(csv);
    print_report();
    printf("CSV written to %s\n", csv_path);
    close(sockfd);
    return 0;
}