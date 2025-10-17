#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h> 


#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <net/ethernet.h>
#include <netinet/if_ether.h> // This header defines 'struct ether_arp'
#include <netinet/ip.h>
#include <netinet/ip6.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <netinet/ip_icmp.h>
#include <net/if_arp.h>

#include "sniffer.h"
#include "utils.h"


static pcap_t *pcap_handle = NULL;
static int packet_id_counter = 0;
static bool is_inspecting = false; // To control inspection-specific output
captured_packet *session[MAX_PACKETS_TO_STORE] = {NULL};
int session_packet_count = 0;
static int link_layer_type = 0;



void analyze_ip_packet(const u_char *packet);
void analyze_arp_packet(const u_char *packet);
void analyze_tcp_segment(const u_char *packet, int ip_payload_len);
void analyze_udp_datagram(const u_char *packet);


void clear_session() {
    for (int i = 0; i < session_packet_count; i++) {
        if (session[i]) {
            free(session[i]->data);
            free(session[i]);
            session[i] = NULL;
        }
    }
    session_packet_count = 0;
}

static void add_packet_to_session(const struct pcap_pkthdr *header, const u_char *packet) {
    if (session_packet_count >= MAX_PACKETS_TO_STORE) return;
    captured_packet *p = malloc(sizeof(captured_packet));
    if (!p) { perror("malloc for captured_packet"); return; }
    p->header = *header;
    p->data = malloc(header->caplen);
    if (!p->data) { free(p); perror("malloc for packet data"); return; }
    memcpy(p->data, packet, header->caplen);
    session[session_packet_count++] = p;
}

void inspect_session() {
    if (session_packet_count == 0) {
        printf("\n[C-Shark] No session has been recorded yet.\n");
        return;
    }
    printf("\n--- Last Session Summary ---\n");
    for (int i = 0; i < session_packet_count; i++) {
        printf("  ID: %d | Time: %ld.%06ld | Length: %d bytes\n",
            i + 1, session[i]->header.ts.tv_sec, session[i]->header.ts.tv_usec, session[i]->header.len);
    }
    printf("--------------------------\n");
    printf("Enter Packet ID to inspect (1-%d): ", session_packet_count);
    int id = 0;
    char input[10];
    if (fgets(input, sizeof(input), stdin) == NULL || sscanf(input, "%d", &id) != 1) {
        // Clear stdin in case of bad input that scanf leaves behind
        int c;
        while ((c = getchar()) != '\n' && c != EOF);
        printf("Invalid input.\n");
        return;
    }
    if (id > 0 && id <= session_packet_count) {
        captured_packet *p = session[id - 1];
        printf("\n--- In-Depth Analysis of Packet #%d ---\n", id);
        is_inspecting = true; // Set the flag before analysis
        analyze_packet(&p->header, p->data, id);
        is_inspecting = false; // Unset the flag after analysis
    } else {
        printf("Invalid Packet ID.\n");
    }
}

//  Main Packet Analysis Function 
void analyze_packet(const struct pcap_pkthdr *header, const u_char *packet, int packet_id) {
    printf("-----------------------------------------\n");
    printf("Packet #%d | Timestamp: %ld.%06ld | Length: %d bytes\n",
        packet_id, header->ts.tv_sec, header->ts.tv_usec, header->len);

    // If in inspection mode, print a full hex dump of the entire frame first
    if (is_inspecting) {
        printf("\n--- Full Packet Hex Dump (%d bytes) ---\n", header->caplen);
        print_payload(packet, header->caplen);
        printf("--- End of Hex Dump ---\n\n");
    }

    uint16_t eth_type;
    const u_char *next_layer_packet;

    
    if (link_layer_type == DLT_EN10MB) { // Standard Ethernet
        const struct ether_header *eth_header = (struct ether_header *)packet;
        printf("L2 (Ethernet): Dst MAC: ");
        print_mac_address(eth_header->ether_dhost);
        printf(" | Src MAC: ");
        print_mac_address(eth_header->ether_shost);
        eth_type = ntohs(eth_header->ether_type);
        next_layer_packet = packet + sizeof(struct ether_header);
    } else if (link_layer_type == DLT_LINUX_SLL) { 
        printf("L2 (Linux SLL): ");
        eth_type = ntohs(*(uint16_t*)(packet + 14));
        next_layer_packet = packet + 16; // Skip the 16-byte SLL header
    } else {
        printf(" | L2: Unsupported link layer type (%d)\n", link_layer_type);
        return;
    }

    if (eth_type == ETHERTYPE_IP) {
        printf(" | EtherType: IPv4 (0x%04x)\n", eth_type);
        analyze_ip_packet(next_layer_packet);
    } else if (eth_type == ETHERTYPE_ARP) {
        printf(" | EtherType: ARP (0x%04x)\n", eth_type);
        analyze_arp_packet(next_layer_packet);
    } else if (eth_type == ETHERTYPE_IPV6) {
        printf(" | EtherType: IPv6 (0x%04x)\n", eth_type);
    } else {
        printf(" | EtherType: Unknown (0x%04x)\n", eth_type);
    }
}

//  Layer 3 Protocol Parsers 

void analyze_ip_packet(const u_char *packet) {
    const struct ip *ip_header = (const struct ip *)packet;
    int ip_header_len = ip_header->ip_hl * 4;
    int ip_total_len = ntohs(ip_header->ip_len);

    printf("L3 (IPv4): Src IP: %s", inet_ntoa(ip_header->ip_src));
    printf(" | Dst IP: %s", inet_ntoa(ip_header->ip_dst));
    printf(" | Protocol: ");
    
    const u_char *transport_layer_packet = packet + ip_header_len;
    int ip_payload_len = ip_total_len - ip_header_len;

    // Print the correct protocol name
    if (ip_header->ip_p == IPPROTO_TCP) {
        printf("TCP (%d)", ip_header->ip_p);
    } else if (ip_header->ip_p == IPPROTO_UDP) {
        printf("UDP (%d)", ip_header->ip_p);
    } else if (ip_header->ip_p == IPPROTO_ICMP) {
        printf("ICMP (%d)", ip_header->ip_p);
    } else {
        printf("Unknown (%d)", ip_header->ip_p);
    }
    
    printf(" | TTL: %d\n", ip_header->ip_ttl);
    printf("           ID: 0x%04x | Total Length: %d | Header Length: %d bytes\n",
           ntohs(ip_header->ip_id), ip_total_len, ip_header_len);

    // Now analyze Layer 4
    if (ip_header->ip_p == IPPROTO_TCP) {
        analyze_tcp_segment(transport_layer_packet, ip_payload_len);
    } else if (ip_header->ip_p == IPPROTO_UDP) {
        analyze_udp_datagram(transport_layer_packet);
    }
}

void analyze_arp_packet(const u_char *packet) {
    const struct ether_arp *arp_packet = (const struct ether_arp *)packet;
    uint16_t op_code = ntohs(arp_packet->ea_hdr.ar_op);
    struct in_addr sender_ip, target_ip;
    memcpy(&sender_ip, arp_packet->arp_spa, 4);
    memcpy(&target_ip, arp_packet->arp_tpa, 4);

    printf("L3 (ARP): Operation: %s (%d) | Sender MAC: ",
        (op_code == ARPOP_REQUEST) ? "Request" : "Reply", op_code);
    print_mac_address(arp_packet->arp_sha);
    printf(" | Sender IP: %s\n", inet_ntoa(sender_ip));

    printf("            Target MAC: ");
    print_mac_address(arp_packet->arp_tha);
    printf(" | Target IP: %s\n", inet_ntoa(target_ip));
}


// Layer 4 Protocol Parsers 
void analyze_tcp_segment(const u_char *packet, int ip_payload_len) {
    const struct tcphdr *tcp_header = (const struct tcphdr *)packet;
    int tcp_header_len = tcp_header->th_off * 4;
    uint16_t src_port = ntohs(tcp_header->th_sport);
    uint16_t dst_port = ntohs(tcp_header->th_dport);

    printf("L4 (TCP): Src Port: %d", src_port);
    if (src_port == 80 || dst_port == 80) printf(" (HTTP)");
    if (src_port == 443 || dst_port == 443) printf(" (HTTPS)");
    if (src_port == 53 || dst_port == 53) printf(" (DNS)");
    printf(" | Dst Port: %d", dst_port);
    if (dst_port == 80 && src_port != 80) printf(" (HTTP)");
    if (dst_port == 443 && src_port != 443) printf(" (HTTPS)");
    if (dst_port == 53 && src_port != 53) printf(" (DNS)");
    
    printf("\n          Seq: %u | Ack: %u", ntohl(tcp_header->th_seq), ntohl(tcp_header->th_ack));

    // Display TCP Flags
    printf(" | Flags: [");
   
    char flags_str[40] = "";
    if (tcp_header->th_flags & TH_FIN) strcat(flags_str, "FIN,");
    if (tcp_header->th_flags & TH_SYN) strcat(flags_str, "SYN,");
    if (tcp_header->th_flags & TH_RST) strcat(flags_str, "RST,");
    if (tcp_header->th_flags & TH_PUSH) strcat(flags_str, "PSH,");
    if (tcp_header->th_flags & TH_ACK) strcat(flags_str, "ACK,");
    if (tcp_header->th_flags & TH_URG) strcat(flags_str, "URG,");
    if (strlen(flags_str) > 0) {
        flags_str[strlen(flags_str) - 1] = '\0'; // Remove last comma
    }
    printf("%s]", flags_str);

    printf("\n          Window: %d | Checksum: 0x%04x | Header Length: %d bytes", 
        ntohs(tcp_header->th_win), ntohs(tcp_header->th_sum), tcp_header_len);
    
    const u_char *payload = packet + tcp_header_len;
    int payload_len = ip_payload_len - tcp_header_len;
    
    if (payload_len > 0) {
        printf("\nL7 (Payload): %d bytes\n", payload_len);
        print_payload(payload, payload_len);
    }
}

void analyze_udp_datagram(const u_char *packet) {
    const struct udphdr *udp_header = (const struct udphdr *)packet;
    uint16_t src_port = ntohs(udp_header->uh_sport);
    uint16_t dst_port = ntohs(udp_header->uh_dport);
    
    printf("L4 (UDP): Src Port: %d | Dst Port: %d", src_port, dst_port);
    if (dst_port == 53 || src_port == 53) printf(" (DNS)");
    printf(" | Length: %d\n", ntohs(udp_header->uh_ulen));
    
    const u_char *payload = packet + 8;
    int payload_len = ntohs(udp_header->uh_ulen) - 8;
    
    if (payload_len > 0) {
        printf("L7 (Payload): %d bytes\n", payload_len);
        print_payload(payload, payload_len);
    }
}


// Pcap Logic 
static void pcap_callback(u_char *user, const struct pcap_pkthdr *header, const u_char *packet) {
    packet_id_counter++;
    analyze_packet(header, packet, packet_id_counter);
    add_packet_to_session(header, packet);
}

static void stop_capture(int signo) {
    if (pcap_handle) {
        printf("\n[C-Shark] Stopping capture...\n");
        pcap_breakloop(pcap_handle);
    }
}

static void start_capture_loop(const char *device, const char *filter_exp) {
    char errbuf[PCAP_ERRBUF_SIZE];
    packet_id_counter = 0;
    
    pcap_handle = pcap_open_live(device, BUFSIZ, 1, 1000, errbuf);
    if (pcap_handle == NULL) {
        fprintf(stderr, "[Error] Couldn't open device %s: %s\n", device, errbuf);
        return;
    }
    link_layer_type = pcap_datalink(pcap_handle);
    if (link_layer_type != DLT_EN10MB && link_layer_type != DLT_LINUX_SLL) {
        fprintf(stderr, "[Error] Unsupported data link type: %s\n", pcap_datalink_val_to_name(link_layer_type));
        pcap_close(pcap_handle);
        return;
    }
    if (filter_exp) {
        struct bpf_program fp;
        if (pcap_compile(pcap_handle, &fp, filter_exp, 0, PCAP_NETMASK_UNKNOWN) == -1) {
            fprintf(stderr, "[Error] Couldn't parse filter %s: %s\n", filter_exp, pcap_geterr(pcap_handle));
            pcap_close(pcap_handle); return;
        }
        if (pcap_setfilter(pcap_handle, &fp) == -1) {
            fprintf(stderr, "[Error] Couldn't install filter %s: %s\n", filter_exp, pcap_geterr(pcap_handle));
            pcap_close(pcap_handle); return;
        }
        pcap_freecode(&fp);
    }
    
    signal(SIGINT, stop_capture);
    printf("\n[C-Shark] Sniffing on %s. Press Ctrl-C to stop...\n", device);
    pcap_loop(pcap_handle, -1, pcap_callback, NULL);
    
    printf("\n[C-Shark] Capture stopped.\n");
    pcap_close(pcap_handle);
    pcap_handle = NULL;
}

void start_sniffing(const char *device) {
    clear_session(); 
    start_capture_loop(device, NULL);
}

void start_filtered_sniffing(const char *device, const char *filter_exp) {
    clear_session(); 
    start_capture_loop(device, filter_exp);
}
