#ifndef SNIFFER_H
#define SNIFFER_H

#include <pcap.h>
#include <stdbool.h>
#define MAX_PACKETS_TO_STORE 1000

// A structure to hold a captured packet's data and metadata
typedef struct {
    struct pcap_pkthdr header; // The header provided by pcap
    u_char *data;              // A deep copy of the packet's raw data
} captured_packet;



// Core sniffing functions
void start_sniffing(const char *device);
void start_filtered_sniffing(const char *device, const char *filter_exp);

// The main analysis function that dissects a single packet
void analyze_packet(const struct pcap_pkthdr *header, const u_char *packet, int packet_id);

// Session management functions
void clear_session();
void inspect_session();

#endif // SNIFFER_H