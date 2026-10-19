/*
 * fujitrace_test -- pure function test, no emulator boot needed: the
 * FujiNet tab's decoder fed synthetic SLIP bytes. Both directions, a reply
 * arriving a byte at a time, frames split across calls, SLIP escapes, half
 * a reply thrown away by the next send, short and long packets, folding of
 * repeated exchanges and of repeated packets, clearing, and the ring's
 * limit.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fujitrace.h"

static int failed = 0;

static void check(const char *what, int ok)
{
    if (!ok) {
        fprintf(stderr, "fujitrace_test: FAILED: %s\n", what);
        failed = 1;
    }
}

static fujitrace t;
static intvdebug_fuji_packet out[FUJITRACE_MAX];

/* A FujiBus packet (6-byte header, then payload) as a SLIP frame. */
static size_t slip(uint8_t dev, uint8_t cmd, const uint8_t *payload,
                   size_t n, uint8_t *dst)
{
    uint8_t raw[1100];
    size_t i, len = 0;
    const size_t total = 6 + n;

    raw[0] = dev;
    raw[1] = cmd;
    raw[2] = (uint8_t)(total & 0xFF);
    raw[3] = (uint8_t)(total >> 8);
    raw[4] = 0x5A; /* checksum: the decoder does not check it */
    raw[5] = 0x00;
    memcpy(raw + 6, payload, n);

    dst[len++] = 0xC0;
    for (i = 0; i < total; i++) {
        if (raw[i] == 0xC0) {
            dst[len++] = 0xDB;
            dst[len++] = 0xDC;
        } else if (raw[i] == 0xDB) {
            dst[len++] = 0xDB;
            dst[len++] = 0xDD;
        } else {
            dst[len++] = raw[i];
        }
    }
    dst[len++] = 0xC0;
    return len;
}

static void send_frame(uint8_t dev, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t f[2300];
    size_t len = slip(dev, cmd, p, n, f);
    fujitrace_feed(&t, 1, f, len);
}

/* As fujinet.c reads a reply: one byte per call. */
static void recv_frame(uint8_t dev, uint8_t cmd, const uint8_t *p, size_t n)
{
    uint8_t f[2300];
    size_t i, len = slip(dev, cmd, p, n, f);
    for (i = 0; i < len; i++)
        fujitrace_feed(&t, 0, f + i, 1);
}

static int copy(void)
{
    return fujitrace_copy(&t, out, FUJITRACE_MAX);
}

static int ends_with(const char *s, const char *tail)
{
    size_t a = strlen(s), b = strlen(tail);
    return a >= b && strcmp(s + a - b, tail) == 0;
}

int main(void)
{
    static const uint8_t slots[4] = { 'T', 'M', 'A', '3' };
    static const uint8_t esc[4] = { 0xC0, 0xDB, 0x01, 0xC0 };
    uint8_t buf[2300], big[200];
    uint64_t serial;
    size_t len, i;
    int n;

    /* ---- empty ---- */
    check("empty trace copies nothing", copy() == 0);

    /* ---- a request, then its reply a byte at a time ---- */
    send_frame(0x70, 0xF2, NULL, 0);
    n = copy();
    check("one request", n == 1);
    check("request direction", out[0].to_host == 0);
    check("request header", out[0].device == 0x70 && out[0].command == 0xF2 &&
                            out[0].length == 6 && out[0].nbytes == 6);
    check("request text",
          strcmp(out[0].text, "Intellivision \xe2\x86\x92 FujiNet 70 F2 "
                              "READ DEVICE SLOTS (6 bytes)") == 0);
    check("request repeat", out[0].repeat == 1);

    recv_frame(0x70, 0x06, slots, sizeof(slots));
    n = copy();
    check("request and reply", n == 2);
    check("reply direction", out[1].to_host == 1);
    check("reply text", strcmp(out[1].text, "FujiNet \xe2\x86\x92 Intellivision "
                                            "70 06 ACK (10 bytes)") == 0);
    check("reply payload", out[1].nbytes == 10 &&
                           memcmp(out[1].data + 6, slots, 4) == 0);
    check("serials rise", out[1].serial > out[0].serial);

    /* ---- one frame over three calls, with SLIP escapes ---- */
    len = slip(0x70, 0xE2, esc, sizeof(esc), buf);
    fujitrace_feed(&t, 1, buf, 3);
    fujitrace_feed(&t, 1, buf + 3, 5); /* splits an escape pair */
    check("nothing until the frame ends", copy() == 2);
    fujitrace_feed(&t, 1, buf + 8, len - 8);
    n = copy();
    check("split frame is one packet", n == 3);
    check("escapes decoded", out[2].nbytes == 10 &&
                             memcmp(out[2].data + 6, esc, 4) == 0);
    check("named", strstr(out[2].text, "E2 SET DEVICE FULLPATH") != NULL);

    /* ---- half a reply, then a send: the half is dropped ---- */
    len = slip(0x70, 0x06, slots, sizeof(slots), buf);
    fujitrace_feed(&t, 0, buf, len / 2);
    send_frame(0x70, 0xFA, NULL, 0);
    recv_frame(0x70, 0x06, slots, 1);
    n = copy();
    check("half reply dropped", n == 5);
    check("then the request", out[3].to_host == 0 && out[3].command == 0xFA);
    check("then a clean reply", out[4].to_host == 1 && out[4].nbytes == 7 &&
                                out[4].data[6] == 'T');

    /* ---- a repeated exchange folds into one pair ---- */
    fujitrace_clear(&t);
    check("clear", copy() == 0);
    send_frame(0x70, 0xFA, NULL, 0);
    recv_frame(0x70, 0x06, slots, 1);
    serial = 0;
    for (i = 0; i < 3; i++) {
        send_frame(0x70, 0xFA, NULL, 0);
        n = copy();
        check("a repeat's request is counted at once",
              n == 2 && out[0].repeat == i + 2 && out[1].repeat == i + 1);
        check("and changes the last serial", out[1].serial > serial);
        serial = out[1].serial;
        recv_frame(0x70, 0x06, slots, 1);
    }
    n = copy();
    check("folded exchange", n == 2 && out[0].repeat == 4 &&
                             out[1].repeat == 4);
    check("count in the text", ends_with(out[0].text, " \xc3\x97" "4") &&
                               ends_with(out[1].text, " \xc3\x97" "4"));

    /* ---- the same request with a different reply is a new exchange ---- */
    send_frame(0x70, 0xFA, NULL, 0);
    recv_frame(0x70, 0x06, slots, 2);
    n = copy();
    check("unfolded", n == 4);
    check("old pair keeps its count", out[0].repeat == 4 &&
                                      out[1].repeat == 4 &&
                                      ends_with(out[0].text, "\xc3\x97" "4"));
    check("new request", out[2].to_host == 0 && out[2].repeat == 1 &&
                         out[2].command == 0xFA &&
                         !strstr(out[2].text, "\xc3\x97"));
    check("new reply", out[3].to_host == 1 && out[3].nbytes == 8);
    check("serials still rise", out[0].serial < out[1].serial &&
                                out[1].serial < out[2].serial &&
                                out[2].serial < out[3].serial);

    /* ---- a request retried with no reply folds too ---- */
    fujitrace_clear(&t);
    send_frame(0x70, 0xF8, NULL, 0);
    send_frame(0x70, 0xF8, NULL, 0);
    send_frame(0x70, 0xF8, NULL, 0);
    n = copy();
    check("retries fold", n == 1 && out[0].repeat == 3 &&
                          strstr(out[0].text, "F8 MOUNT IMAGE") != NULL);

    /* ---- a short frame, and a long one ---- */
    fujitrace_clear(&t);
    {
        static const uint8_t junk[3] = { 0xC0, 0x41, 0x42 };
        static const uint8_t end = 0xC0;
        fujitrace_feed(&t, 0, junk, 3);
        fujitrace_feed(&t, 0, &end, 1);
    }
    n = copy();
    check("short frame", n == 1 && out[0].device == -1 &&
                         out[0].command == -1 && out[0].nbytes == 2);
    check("short text", strcmp(out[0].text, "FujiNet \xe2\x86\x92 "
                                            "Intellivision 2 bytes") == 0);

    for (i = 0; i < sizeof(big); i++)
        big[i] = (uint8_t)(i + 1);
    recv_frame(0xFF, 0x57, big, sizeof(big));
    big[sizeof(big) - 1] ^= 0xFF; /* differs only past the first 64 bytes */
    recv_frame(0xFF, 0x57, big, sizeof(big));
    n = copy();
    check("long frames differing late stay apart", n == 3 &&
          out[1].repeat == 1 && out[2].repeat == 1);
    check("long frame keeps its first bytes", out[1].nbytes == 206 &&
          out[1].data[6] == 1 && out[1].data[63] == 58);
    check("long frame named", strstr(out[1].text, "FF 57 WRITE (206 bytes)") != NULL);

    /* ---- the ring keeps the newest ---- */
    fujitrace_clear(&t);
    for (i = 0; i < FUJITRACE_MAX + 100; i++) {
        uint8_t id[2] = { (uint8_t)(i & 0xFF), (uint8_t)(i >> 8) };
        send_frame(0x70, 0x52, id, 2);
    }
    n = copy();
    check("ring full", n == FUJITRACE_MAX);
    check("oldest dropped", out[0].data[6] == 100 && out[0].data[7] == 0);
    check("newest last", out[n - 1].data[6] == (uint8_t)((FUJITRACE_MAX + 99) & 0xFF));
    n = fujitrace_copy(&t, out, 10);
    check("copy takes the newest", n == 10 &&
          out[9].data[6] == (uint8_t)((FUJITRACE_MAX + 99) & 0xFF));
    check("copy of none", fujitrace_copy(&t, out, 0) == 0);

    if (failed)
        return 1;
    printf("fujitrace_test: OK\n");
    return 0;
}
