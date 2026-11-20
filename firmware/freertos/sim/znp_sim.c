/* SPDX-License-Identifier: MIT
 * Host simulator of the FreeRTOS coordinator's ZNP interface.
 *
 *   znp_sim <tcp port> [flash image file] [product]
 *
 * Runs the SAME portable code as the firmware (znp.c, zb_coord, MAC,
 * nv_log, NPI framing) on the PC and serves MT frames over TCP, so that
 * the real zigbee-herdsman ZStackAdapter can connect with
 * `tcp://127.0.0.1:<port>`. What is simulated, and only that:
 *   - Flash: a RAM copy of the 32 KiB nv_log store, saved to the image file
 *     after every change when one is given (survives simulator restarts);
 *   - Radio: every transmission "succeeds" without any receiver (no device
 *     exists in this simulation, so unicasts are never attempted by tests);
 *   - TRNG: the C library PRNG seeded from the clock;
 *   - Reset: SYS_RESET_REQ re-runs znp_boot() on the same Flash.
 * Product id: the third argument (default 0xFE, experimental); the
 * acceptance test passes 1 to exercise herdsman's Z-Stack 3.x path.
 */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif
#include "npi.h"
#include "nv_log.h"
#include "znp.h"

#define STORE_BASE 0x48000u
#define BANK_SIZE 0x4000u
#define SECTOR 0x2000u

static uint8_t flash[2 * BANK_SIZE];
static const char *image_path;
static bool format_store;
static nv_log_t store;
static zb_coord_t coord;
static znp_t znp;
static npi_parser parser;
static sock_t client = INVALID_SOCKET;
static bool reset_requested, tx_pending;
static uint32_t tx_count;

static uint32_t now_ms(void) {
#ifdef _WIN32
    return (uint32_t)GetTickCount();
#else
    struct timeval tv;
    gettimeofday(&tv, 0);
    return (uint32_t)(tv.tv_sec * 1000u + tv.tv_usec / 1000u);
#endif
}

static void save_image(void) {
    if (!image_path) return;
    FILE *f = fopen(image_path, "wb");
    if (!f) return;
    fwrite(flash, 1, sizeof flash, f);
    fclose(f);
}

static bool fl_read(void *ctx, uint32_t addr, void *buf, size_t len) {
    (void)ctx;
    if (addr < STORE_BASE || addr + len > STORE_BASE + sizeof flash) return false;
    memcpy(buf, flash + (addr - STORE_BASE), len);
    return true;
}
static bool fl_program(void *ctx, uint32_t addr, const void *data, size_t len) {
    (void)ctx;
    if (addr < STORE_BASE || addr + len > STORE_BASE + sizeof flash) return false;
    const uint8_t *d = data;
    for (size_t i = 0; i < len; i++) flash[addr - STORE_BASE + i] &= d[i];   /* NOR: 1 -> 0 only */
    save_image();
    return true;
}
static bool fl_erase(void *ctx, uint32_t addr) {
    (void)ctx;
    if (addr < STORE_BASE || addr + SECTOR > STORE_BASE + sizeof flash || (addr % SECTOR)) return false;
    memset(flash + (addr - STORE_BASE), 0xFF, SECTOR);
    save_image();
    return true;
}

static int radio_tx(void *ctx, const uint8_t *mpdu, uint8_t len, bool wait_ack) {
    (void)ctx; (void)mpdu; (void)len; (void)wait_ack;
    tx_pending = true;
    tx_count++;
    return 0;
}
static void radio_source_match(void *ctx, bool extended, uint64_t address, bool pending) {
    (void)ctx; (void)extended; (void)address; (void)pending;
}
static const mac_radio_ops_t RADIO = {radio_tx, radio_source_match, 0};

static void send_frame(void *ctx, uint8_t cmd0, uint8_t cmd1, const uint8_t *payload, uint8_t len) {
    (void)ctx;
    npi_frame f = {cmd0, cmd1, len, {0}};
    memcpy(f.payload, payload, len);
    uint8_t wire[NPI_MAX_FRAME];
    size_t n = npi_encode(wire, sizeof wire, &f);
    if (client != INVALID_SOCKET && n) send(client, (const char *)wire, (int)n, 0);
}
static void plat_reset(void *ctx) { (void)ctx; reset_requested = true; }
static bool plat_random(void *ctx, uint8_t out[16]) {
    (void)ctx;
    for (unsigned i = 0; i < 16; i++) out[i] = (uint8_t)(rand() >> 4);
    return true;
}
static void plat_network_up(void *ctx, uint8_t channel) {
    (void)ctx;
    fprintf(stderr, "[sim] network up on channel %u\n", channel);
}
static int8_t plat_tx_power(void *ctx, int8_t dbm) {
    (void)ctx;
    return dbm > 20 ? 20 : dbm < -20 ? -20 : dbm;
}

/* Green Power TempMaster: no radio here, the calls are logged (one line
 * each, read back by tools/znp_sim_gp.cjs). */
static bool plat_gp_tx(void *ctx, const uint8_t *mpdu, uint8_t len) {
    (void)ctx;
    fprintf(stderr, "[sim] gp_tx ");
    for (unsigned i = 0; i < len; i++) fprintf(stderr, "%02x", mpdu[i]);
    fprintf(stderr, "\n");
    fflush(stderr);
    return true;
}
static bool plat_gp_channel(void *ctx, uint8_t channel) {
    (void)ctx;
    fprintf(stderr, "[sim] gp_channel %u\n", channel);
    fflush(stderr);
    return true;
}

static znp_platform_t platform = {
    .send = send_frame, .reset = plat_reset, .random = plat_random, .network_up = plat_network_up,
    .set_tx_power = plat_tx_power, .revision = 20261118u, .product = ZNP_PRODUCT_EXPERIMENTAL,
    .major = 2, .minor = 7, .maint = 1, .gp_tx = plat_gp_tx, .gp_channel = plat_gp_channel,
};

/* Frames "heard on the air": <image>.inject holds one MPDU (hex, no FCS) per
 * line; it is read, deleted, and each frame given to the coordinator. */
static void inject_frames(void) {
    if (!image_path) return;
    char name[1024];
    snprintf(name, sizeof name, "%s.inject", image_path);
    FILE *f = fopen(name, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof line, f)) {
        uint8_t mpdu[127];
        unsigned n = 0, b;
        for (const char *c = line; n < sizeof mpdu && sscanf(c, "%2x", &b) == 1; c += 2) mpdu[n++] = (uint8_t)b;
        if (n >= 3) {
            fprintf(stderr, "[sim] inject %u bytes\n", n);
            zb_coord_rx(&coord, mpdu, (uint8_t)n, 200, -50);
        }
    }
    fclose(f);
    remove(name);
}

static void on_frame(void *ctx, const npi_frame *f) {
    (void)ctx;
    znp_handle(&znp, f->cmd0, f->cmd1, f->payload, f->length);
}

static const nv_flash_ops_t FLASH_OPS = {fl_read, fl_program, fl_erase, 0};

static void boot(uint8_t reason) {
    if (!nv_log_mount(&store, &FLASH_OPS, STORE_BASE, BANK_SIZE, SECTOR)) {
        /* Never formatted implicitly: an explicit --format-store is needed. */
        fprintf(stderr, "[sim] store not mountable (status %d)%s\n", (int)store.mount_status,
                format_store ? ", formatting as requested" : "; restart with --format-store to erase it");
        if (!format_store || !nv_log_format(&store)) exit(3);
    }
    znp_boot(&znp, &platform, &coord, &RADIO, &store, 0x00124B00DEADBEEFull, (uint32_t)rand(), reason);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: znp_sim <port> [image] [product]\n"); return 2; }
    int port = atoi(argv[1]);
    if (argc > 2 && strcmp(argv[2], "-")) image_path = argv[2];
    if (argc > 3) platform.product = (uint8_t)strtoul(argv[3], 0, 0);
    for (int i = 4; i < argc; i++) if (!strcmp(argv[i], "--format-store")) format_store = true;
    srand((unsigned)time(0));
    memset(flash, 0xFF, sizeof flash);
    if (image_path) {
        FILE *f = fopen(image_path, "rb");
        if (f) { size_t n = fread(flash, 1, sizeof flash, f); (void)n; fclose(f); }
    }
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_t listener = socket(AF_INET, SOCK_STREAM, 0);
    int yes = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof yes);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&addr, sizeof addr) || listen(listener, 1)) {
        fprintf(stderr, "[sim] cannot listen on %d\n", port);
        return 1;
    }
    socklen_t alen = sizeof addr;
    getsockname(listener, (struct sockaddr *)&addr, &alen);   /* port 0: chosen by the OS */
    fprintf(stderr, "[sim] listening on 127.0.0.1:%d\n", ntohs(addr.sin_port));
    fflush(stderr);
    npi_init(&parser, on_frame, 0);
    boot(0);
    for (;;) {
        if (client == INVALID_SOCKET) {
            client = accept(listener, 0, 0);
            if (client == INVALID_SOCKET) continue;
            fprintf(stderr, "[sim] host connected\n");
            npi_init(&parser, on_frame, 0);
        }
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(client, &rd);
        struct timeval tv = {0, 5000};
        int ready = select((int)client + 1, &rd, 0, 0, &tv);
        if (ready > 0) {
            uint8_t buf[512];
            int n = recv(client, (char *)buf, sizeof buf, 0);
            if (n <= 0) {
                fprintf(stderr, "[sim] host disconnected\n");
                CLOSESOCK(client);
                client = INVALID_SOCKET;
                continue;
            }
            for (int i = 0; i < n; i++) npi_feed(&parser, buf[i], now_ms());
        }
        npi_expire(&parser, now_ms());
        if (tx_pending) {
            tx_pending = false;
            zb_coord_tx_done(&coord, MAC_TX_OK, false);
        }
        inject_frames();
        zb_coord_tick(&coord, now_ms());
        znp_poll(&znp);
        if (reset_requested) {
            reset_requested = false;
            fprintf(stderr, "[sim] reset\n");
            boot(1);
        }
    }
}
