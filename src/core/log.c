#include "core/pf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

enum pf_log_level pf_log_threshold = PF_LOG_INFO;

static const char *const level_tag[] = {"ERR ", "WARN", "INFO", "DBG "};

void pf_logf(enum pf_log_level lvl, const char *fmt, ...)
{
    if (lvl > pf_log_threshold)
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", level_tag[lvl]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

void pf_panic(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[PANIC] ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    abort();
}

void pf_hexdump(const void *buf, size_t len)
{
    const uint8_t *p = buf;
    for (size_t off = 0; off < len; off += 16) {
        printf("  %04zx  ", off);
        for (size_t i = 0; i < 16; i++) {
            if (off + i < len)
                printf("%02x ", p[off + i]);
            else
                fputs("   ", stdout);
            if (i == 7)
                fputc(' ', stdout);
        }
        fputs(" |", stdout);
        for (size_t i = 0; i < 16 && off + i < len; i++) {
            uint8_t c = p[off + i];
            fputc(c >= 0x20 && c < 0x7f ? c : '.', stdout);
        }
        fputs("|\n", stdout);
    }
    fflush(stdout);
}

const char *pf_ip4_str(uint32_t ip, char buf[16])
{
    snprintf(buf, 16, "%u.%u.%u.%u", (ip >> 24) & 0xff, (ip >> 16) & 0xff, (ip >> 8) & 0xff,
             ip & 0xff);
    return buf;
}

bool pf_ip4_parse(const char *s, uint32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4)
        return false;
    if (a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return true;
}

const char *pf_mac_str(const uint8_t mac[6], char buf[18])
{
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
             mac[5]);
    return buf;
}
