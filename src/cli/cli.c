#include "cli/cli.h"
#include "arp/arp.h"
#include "core/stack.h"
#include "netdev/netdev.h"
#include "route/fib.h"
#include "tcp/tcp.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define CLI_PROMPT "pf> "

static void cli_printf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void cli_printf(int fd, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n > 0) {
        ssize_t w = write(fd, buf, (size_t)PF_MIN(n, (int)sizeof(buf)));
        (void)w;
    }
}

int cli_open(struct pf_stack *stack, const char *path)
{
    struct cli_server *c = calloc(1, sizeof(*c));
    PF_ASSERT(c);
    for (int i = 0; i < CLI_MAX_CLIENTS; i++)
        c->clients[i].fd = -1;

    c->listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (c->listen_fd < 0) {
        free(c);
        return -1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
    snprintf(c->path, sizeof(c->path), "%s", path);
    unlink(path);
    if (bind(c->listen_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(c->listen_fd, 2) < 0) {
        PF_ERR("cli: bind/listen %s: %s", path, strerror(errno));
        close(c->listen_fd);
        free(c);
        return -1;
    }
    stack->cli = c;
    PF_INFO("cli: listening on %s", path);
    return 0;
}

void cli_close(struct pf_stack *stack)
{
    struct cli_server *c = stack->cli;
    if (!c)
        return;
    for (int i = 0; i < CLI_MAX_CLIENTS; i++)
        if (c->clients[i].fd >= 0)
            close(c->clients[i].fd);
    close(c->listen_fd);
    unlink(c->path);
    free(c);
    stack->cli = NULL;
}

int cli_listen_fd(struct pf_stack *stack)
{
    return stack->cli ? stack->cli->listen_fd : -1;
}

int cli_client_fd(struct pf_stack *stack, int slot)
{
    return stack->cli ? stack->cli->clients[slot].fd : -1;
}

void cli_accept(struct pf_stack *stack)
{
    struct cli_server *c = stack->cli;
    int fd = accept(c->listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    for (int i = 0; i < CLI_MAX_CLIENTS; i++) {
        if (c->clients[i].fd < 0) {
            c->clients[i].fd = fd;
            c->clients[i].len = 0;
            cli_printf(fd, "PacketForge CLI — type 'help'\n%s", CLI_PROMPT);
            return;
        }
    }
    close(fd); /* table full */
}

/* ---- commands ----------------------------------------------------------- */

static const char *tcp_state_name(enum tcp_state s)
{
    static const char *const names[] = {"CLOSED",  "LISTEN",   "SYNSENT",  "SYNRCVD",
                                        "ESTAB",   "FINWAIT1", "FINWAIT2", "CLOSEWAIT",
                                        "CLOSING", "LASTACK",  "TIMEWAIT"};
    return names[s];
}

static void cmd_show_interfaces(struct pf_stack *stack, int fd)
{
    for (int i = 0; i < stack->ndevs; i++) {
        const struct netdev *d = stack->devs[i];
        char ip[16], mask[16], mac[18];
        cli_printf(fd, "%s is up, line protocol is up\n", d->name);
        cli_printf(fd, "  Hardware address %s, MTU %u\n", pf_mac_str(d->mac, mac), d->mtu);
        if (d->ip)
            cli_printf(fd, "  Internet address %s, mask %s\n", pf_ip4_str(d->ip, ip),
                       pf_ip4_str(d->mask, mask));
        cli_printf(fd, "  %llu packets input, %llu bytes, %llu drops\n",
                   (unsigned long long)d->st.rx_pkts, (unsigned long long)d->st.rx_bytes,
                   (unsigned long long)d->st.rx_drops);
        cli_printf(fd, "  %llu packets output, %llu bytes, %llu errors\n",
                   (unsigned long long)d->st.tx_pkts, (unsigned long long)d->st.tx_bytes,
                   (unsigned long long)d->st.tx_errs);
    }
}

struct route_print_ctx {
    int fd;
};

static void print_route(const struct fib_entry *e, void *arg)
{
    struct route_print_ctx *ctx = arg;
    char pfx[16], nh[16];
    pf_ip4_str(e->prefix, pfx);
    if (e->connected)
        cli_printf(ctx->fd, "C    %s/%u is directly connected, %s\n", pfx, e->plen, e->dev->name);
    else
        cli_printf(ctx->fd, "S    %s/%u via %s, %s\n", pfx, e->plen, pf_ip4_str(e->next_hop, nh),
                   e->dev->name);
}

static void cmd_show_ip_route(struct pf_stack *stack, int fd)
{
    cli_printf(fd, "Codes: C - connected, S - static\n");
    struct route_print_ctx ctx = {fd};
    fib_walk(&stack->fib, print_route, &ctx);
}

static void cmd_show_ip_traffic(struct pf_stack *stack, int fd)
{
    for (size_t i = 0; i < pf_stats_count(); i++)
        cli_printf(fd, "  %-28s %llu\n", pf_stats_name(i),
                   (unsigned long long)pf_stats_get(&stack->stats, i));
}

static void cmd_show_tcp_brief(struct pf_stack *stack, int fd)
{
    cli_printf(fd, "%-22s %-22s %s\n", "Local Address", "Foreign Address", "(state)");
    for (int i = 0; i < TCP_MAX_CONNS; i++) {
        const struct tcp_cb *t = &stack->tcp.tcbs[i];
        if (!t->active)
            continue;
        char l[16], r[16];
        cli_printf(fd, "%-15s:%-6u %-15s:%-6u %s\n", pf_ip4_str(t->local_ip, l), t->local_port,
                   pf_ip4_str(t->remote_ip, r), t->remote_port, tcp_state_name(t->state));
    }
}

static void cmd_show_arp(struct pf_stack *stack, int fd)
{
    cli_printf(fd, "%-16s %-18s %-10s %s\n", "Address", "Hardware Addr", "State", "Interface");
    for (size_t i = 0; i < ARP_CACHE_SIZE; i++) {
        const struct arp_entry *e = &stack->arp.entries[i];
        if (e->state == ARP_FREE)
            continue;
        char ip[16], mac[18];
        cli_printf(fd, "%-16s %-18s %-10s %s\n", pf_ip4_str(e->ip, ip),
                   e->state == ARP_RESOLVED ? pf_mac_str(e->mac, mac) : "incomplete",
                   e->state == ARP_RESOLVED ? "resolved" : "pending", e->dev ? e->dev->name : "-");
    }
}

/* Contiguous netmask → prefix length; -1 on a non-contiguous mask. */
static int mask_to_plen(uint32_t mask)
{
    int plen = 0;
    while (plen < 32 && (mask & 0x80000000u)) {
        mask <<= 1;
        plen++;
    }
    return mask == 0 ? plen : -1;
}

static void cmd_ip_route(struct pf_stack *stack, int fd, char *args, bool add)
{
    char *save = NULL;
    const char *p_s = strtok_r(args, " \t", &save);
    const char *m_s = strtok_r(NULL, " \t", &save);
    const char *n_s = strtok_r(NULL, " \t", &save);

    uint32_t prefix, mask, nh = 0;
    if (!p_s || !m_s || !pf_ip4_parse(p_s, &prefix) || !pf_ip4_parse(m_s, &mask) ||
        (add && (!n_s || !pf_ip4_parse(n_s, &nh)))) {
        cli_printf(fd, "%% usage: %sip route <prefix> <mask> %s\n", add ? "" : "no ",
                   add ? "<next-hop>" : "[next-hop]");
        return;
    }
    int plen = mask_to_plen(mask);
    if (plen < 0) {
        cli_printf(fd, "%% non-contiguous mask\n");
        return;
    }

    if (add) {
        if (fib_add_via(stack, prefix, (uint8_t)plen, nh) < 0)
            cli_printf(fd, "%% next hop %s not reachable via a connected route\n", n_s);
    } else {
        if (fib_del(&stack->fib, prefix, (uint8_t)plen) < 0)
            cli_printf(fd, "%% no such route\n");
    }
}

static void cli_dispatch(struct pf_stack *stack, int fd, char *line)
{
    /* Normalize: strip leading blanks, collapse the verb. */
    while (*line == ' ' || *line == '\t')
        line++;
    size_t n = strlen(line);
    while (n && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' '))
        line[--n] = '\0';
    if (n == 0)
        return;

    if (strcmp(line, "show interfaces") == 0 || strcmp(line, "show int") == 0) {
        cmd_show_interfaces(stack, fd);
    } else if (strcmp(line, "show ip route") == 0) {
        cmd_show_ip_route(stack, fd);
    } else if (strcmp(line, "show ip traffic") == 0) {
        cmd_show_ip_traffic(stack, fd);
    } else if (strcmp(line, "show tcp brief") == 0) {
        cmd_show_tcp_brief(stack, fd);
    } else if (strcmp(line, "show arp") == 0) {
        cmd_show_arp(stack, fd);
    } else if (strncmp(line, "ip route ", 9) == 0) {
        cmd_ip_route(stack, fd, line + 9, true);
    } else if (strncmp(line, "no ip route ", 12) == 0) {
        cmd_ip_route(stack, fd, line + 12, false);
    } else if (strcmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        cli_printf(fd, "  show interfaces | show ip route | show ip traffic\n"
                       "  show tcp brief  | show arp\n"
                       "  ip route <prefix> <mask> <next-hop>\n"
                       "  no ip route <prefix> <mask>\n"
                       "  exit\n");
    } else if (strcmp(line, "exit") == 0 || strcmp(line, "quit") == 0) {
        /* handled by caller via return code 1; keep simple: close below */
        cli_printf(fd, "bye\n");
    } else {
        cli_printf(fd, "%% unknown command: %s\n", line);
    }
}

void cli_client_input(struct pf_stack *stack, int slot)
{
    struct cli_server *c = stack->cli;
    int fd = c->clients[slot].fd;
    char buf[256];
    ssize_t r = read(fd, buf, sizeof(buf));
    if (r <= 0) {
        close(fd);
        c->clients[slot].fd = -1;
        return;
    }
    for (ssize_t i = 0; i < r; i++) {
        char ch = buf[i];
        if (ch == '\n') {
            c->clients[slot].line[c->clients[slot].len] = '\0';
            char *line = c->clients[slot].line;
            bool is_exit = strncmp(line, "exit", 4) == 0 || strncmp(line, "quit", 4) == 0;
            cli_dispatch(stack, fd, line);
            c->clients[slot].len = 0;
            if (is_exit) {
                close(fd);
                c->clients[slot].fd = -1;
                return;
            }
            cli_printf(fd, "%s", CLI_PROMPT);
        } else if (c->clients[slot].len < CLI_LINE_MAX - 1) {
            c->clients[slot].line[c->clients[slot].len++] = ch;
        }
    }
}
