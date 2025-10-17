#include <stdio.h>
#include <ctype.h>
#include "utils.h"

void print_mac_address(const u_char *addr) {
    printf("%02x:%02x:%02x:%02x:%02x:%02x",
           addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

void print_payload(const u_char *payload, int len) {
    if (payload == NULL || len <= 0) {
        printf("    (No Payload Data)\n");
        return;
    }

    const int line_width = 16;
    int i;
    int remaining = len;

    for (i = 0; i < len; i += line_width) {
        // Print hex offset
        printf("    %04x  ", i);

        // Print hex values
        for (int j = 0; j < line_width; j++) {
            if (i + j < len) {
                printf("%02x ", payload[i + j]);
            } else {
                printf("   ");
            }
        }
        printf(" ");

        // Print ASCII characters
        for (int j = 0; j < line_width; j++) {
            if (i + j < len) {
                if (isprint(payload[i + j])) {
                    printf("%c", payload[i + j]);
                } else {
                    printf(".");
                }
            }
        }
        printf("\n");
        remaining -= line_width;
    }
}