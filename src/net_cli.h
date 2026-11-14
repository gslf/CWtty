/*
 * net_cli.h - The rooms from the command line, without the interface:
 * --net-listen prints what is keyed in a room, --net-send keys a text.
 */
#ifndef CWTTY_NET_CLI_H
#define CWTTY_NET_CLI_H

typedef struct {
    int  send; /* 0: listen */
    char text[512];
    char room[64];
    char call[32];
    char password[128];
    char config[1024];
} net_cli_options;

int net_cli(const net_cli_options *o);

#endif
