# C-Shark: The Terminal Packet Sniffer
Layer 2 (The Physical Envelope): The Ethernet Frame. This has the local network addresses (MAC addresses) of the physical sender and receiver.

Layer 3 (The Inter-City Envelope): The IP Packet. This has the global internet addresses (IP addresses) of the original source and final destination. It tells routers where to send the packet.

Layer 4 (The Department/Port Envelope): The TCP/UDP Segment. This has port numbers, which are like apartment numbers for applications on a computer. It tells the operating system which program (e.g., Chrome, a game, a DNS client) should receive the data.

Layer 7 (The Letter Inside): The Payload. This is the actual data your application is sending or receiving (e.g., part of a webpage, a DNS query, a message)


Network data is sent in small chunks called packets. Each packet is structured like a set of nested envelopes (or layers). Your job is to "open" each envelope to read the information on it, then open the next one inside until you get to the actual data



fnss

pcap_findalldevs(): Gets a linked list of all network interfaces.

pcap_freealldevs(): Frees the memory allocated by the function above.

pcap_open_live(): Opens a specific interface to start a capture session.

pcap_loop(): Starts the main capture loop and calls your function for each packet.

pcap_breakloop(): Stops the capture loop gracefully.


