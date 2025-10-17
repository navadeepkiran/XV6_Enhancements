#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pcap.h>
#include "sniffer.h"
#include <signal.h>

#define MAX_DEVICES 20
#define MAX_DEVICE_NAME_LEN 100


void print_main_menu(const char *device);
const char* select_device();
void handle_filtering(const char *selected_device);


static void clear_stdin() {
    int c;
    while ((c = getchar()) != '\n' && c != EOF);
}


void print_main_menu(const char *device) {
    printf("\n[C-Shark] Interface '%s' selected. What's next?\n\n", device);
    printf("1. Start Sniffing (All Packets)\n");
    printf("2. Start Sniffing (With Filters)\n");
    printf("3. Inspect Last Session\n");
    printf("4. Exit C-Shark\n\n");
    printf("Select an option (1-4): ");
}

const char* select_device() {
    pcap_if_t *alldevs, *d;
    char errbuf[PCAP_ERRBUF_SIZE];
    static char device_names[MAX_DEVICES][MAX_DEVICE_NAME_LEN];
    int i = 0;

    printf("[C-Shark] The Command-Line Packet Predator\n");
    printf("==============================================\n");
    printf("[C-Shark] Searching for available interfaces... Found!\n\n");

    if (pcap_findalldevs(&alldevs, errbuf) == -1) {
        fprintf(stderr, "Error in pcap_findalldevs: %s\n", errbuf);
        return NULL;
    }

    for (d = alldevs; d != NULL && i < MAX_DEVICES; d = d->next) {
        printf("%d. %s\n", i + 1, d->name);
        strncpy(device_names[i], d->name, MAX_DEVICE_NAME_LEN - 1);
        device_names[i][MAX_DEVICE_NAME_LEN - 1] = '\0';
        i++;
    }
    int dev_count = i;
    pcap_freealldevs(alldevs);

    if (dev_count == 0) {
        printf("No interfaces found. Make sure you are running with sudo.\n");
        return NULL;
    }

    printf("\nSelect an interface to sniff (1-%d): ", dev_count);
    int choice = 0;
    char input_buffer[16];
    
    if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) { 
        printf("\n[C-Shark] Exiting...\n");                          
        exit(0);                                                     
    }
    if ( sscanf(input_buffer, "%d", &choice) != 1) {
        printf("Invalid input.\n");
        return NULL;
    }

    if (choice > 0 && choice <= dev_count) {
        return device_names[choice - 1];
    } else {
        printf("Invalid selection.\n");
        return NULL;
    }
}

// Filtering Logic 
void handle_filtering(const char *selected_device) {
    printf("\n--- Filter Options ---\n");
    printf("1. TCP\n2. UDP\n3. ARP\n4. DNS (Port 53)\n5. HTTP (Port 80)\n6. HTTPS (Port 443)\n\n");
    printf("Select a filter (1-6): ");

    int choice = 0;
    const char *filter_exp = NULL;
    char input_buffer[16];
    
    if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) { 
        printf("\n[C-Shark] Exiting...\n");                         
        clear_session();                                             
        exit(0);                                                    
    }
    if ( sscanf(input_buffer, "%d", &choice) != 1) {
        printf("Invalid input.\n");
        return;
    }
    
    switch(choice) {
        case 1: filter_exp = "tcp"; break;
        case 2: filter_exp = "udp"; break;
        case 3: filter_exp = "arp"; break;
        case 4: filter_exp = "udp port 53"; break;
        case 5: filter_exp = "tcp port 80"; break;
        case 6: filter_exp = "tcp port 443"; break;
        default: printf("Invalid filter option.\n"); return;
    }

    start_filtered_sniffing(selected_device, filter_exp);
}

// Main Program Loop 
int main() {
    const char *selected_device = select_device();
    if (selected_device == NULL) {
        return 1;
    }

    int choice = 0;
    while (1) {
        print_main_menu(selected_device);
        
        char input_buffer[16];
        if (fgets(input_buffer, sizeof(input_buffer), stdin) == NULL) {
            printf("\n[C-Shark] Exiting...\n");
            break;
        }

        if (sscanf(input_buffer, "%d", &choice) != 1) {
            printf("Invalid input. Please enter a number.\n");
           
            continue;
        }

        switch (choice) {
            case 1:
                start_sniffing(selected_device);
                break;
            case 2:
                handle_filtering(selected_device);
                break;
            case 3:
                inspect_session();
                break;
            case 4:
                printf("\n[C-Shark] Goodbye!\n");
                clear_session(); 
                return 0;
            default:
                printf("Invalid option. Please choose between 1 and 4.\n");
                break;
        }
    }
    
    clear_session();
    return 0;
}