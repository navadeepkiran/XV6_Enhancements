#ifndef UTILS_H
#define UTILS_H

#include <pcap.h>

// --- Function Prototypes for Utility Functions ---

// Prints the payload in a nice hex and ASCII format
void print_payload(const u_char *payload, int len);

// Prints a MAC address from a byte array
void print_mac_address(const u_char *addr);

#endif // UTILS_H