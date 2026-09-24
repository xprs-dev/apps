/*
 * rtsp_live -- the wapp's own RTSP client, on a real socket, against a real
 * camera. Not part of the test run (it needs a camera and credentials); it
 * is how the protocol was developed without a camera inside the wasm
 * sandbox:
 *
 *   cc -I.. -o /tmp/rtsp_live tests/native/rtsp_live.c rtsp.c md5.c wire.c
 *   /tmp/rtsp_live 192.168.1.9 554 /h264Preview_01_sub admin <password> out.h264
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "../../rtsp.h"

static int g_fd;
static FILE *g_out;
static unsigned g_frames, g_bytes;

static int io_send(void *u, const char *b, unsigned n)
{
    (void)u;
    return (int)write(g_fd, b, n);
}
static int io_recv(void *u, char *b, unsigned cap)
{
    (void)u;
    ssize_t n = read(g_fd, b, cap);
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
    if (n == 0) return -1;
    return (int)n;
}
static unsigned long long io_now(void *u)
{
    (void)u;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ULL + (unsigned)(ts.tv_nsec / 1000000);
}
static void on_au(void *u, const unsigned char *au, unsigned len)
{
    (void)u;
    g_frames++;
    g_bytes += len;
    if (g_out) fwrite(au, 1, len, g_out);
}

int main(int argc, char **argv)
{
    if (argc < 6) { fprintf(stderr, "usage: %s host port path user pass [out.h264] [seconds]\n", argv[0]); return 2; }
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)atoi(argv[2]));
    inet_pton(AF_INET, argv[1], &a.sin_addr);
    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(g_fd, (struct sockaddr *)&a, sizeof a) != 0) { perror("connect"); return 1; }
    fcntl(g_fd, F_SETFL, O_NONBLOCK);
    if (argc > 6) g_out = fopen(argv[6], "wb");

    rtsp_io io = { io_send, io_recv, io_now, 0 };
    rtsp_t r;
    rtsp_begin(&r, &io, argv[1], atoi(argv[2]), argv[3], argv[4], argv[5], on_au, 0);
    unsigned long long t0 = io_now(0);
    unsigned long long run_ms = argc > 7 ? (unsigned long long)atoi(argv[7]) * 1000ULL : 8000ULL;
    unsigned long long last = t0;
    while (io_now(0) - t0 < run_ms) {
        if (rtsp_pump(&r) == RTSP_DONE) break;
        if (io_now(0) - last > 5000ULL) {
            last = io_now(0);
            printf("  %llus: %u frames, %u bytes\n",
                   (unsigned long long)((io_now(0) - t0) / 1000), g_frames, g_bytes);
            fflush(stdout);
        }
        usleep(20000);
    }
    printf("state=%d frames=%u bytes=%u why=%s\n", r.state, g_frames, g_bytes,
           r.why[0] ? r.why : "(still running)");
    if (g_out) fclose(g_out);
    close(g_fd);
    return r.state == RTSP_STREAM || g_frames ? 0 : 1;
}
