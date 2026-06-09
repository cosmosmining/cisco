/* IOS-flavored CLI over a UNIX-domain stream socket. */
#ifndef PF_CLI_CLI_H
#define PF_CLI_CLI_H

#include "core/pf.h"

#define CLI_MAX_CLIENTS 4
#define CLI_LINE_MAX    256

struct pf_stack;

struct cli_server {
    int listen_fd;
    char path[108];
    struct {
        int fd; /* -1 = free */
        char line[CLI_LINE_MAX];
        size_t len;
    } clients[CLI_MAX_CLIENTS];
};

/* Bind and listen on `path` (unlinked first). Returns 0/-1. */
int cli_open(struct pf_stack *stack, const char *path);
void cli_close(struct pf_stack *stack);

/* Event-loop integration (called from loop_linux.c). */
int cli_listen_fd(struct pf_stack *stack); /* -1 when CLI disabled */
int cli_client_fd(struct pf_stack *stack, int slot);
void cli_accept(struct pf_stack *stack);
void cli_client_input(struct pf_stack *stack, int slot);

#endif /* PF_CLI_CLI_H */
