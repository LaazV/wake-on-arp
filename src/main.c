#include <errno.h>
#include <netdb.h>
#include <stdio.h>		// for standard things
#include <stdlib.h>		// malloc
#include <string.h>		// strlen
#include <stdbool.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>				// provides declarations for ip header
#include <netinet/if_ether.h>		// for ETH_P_ALL
#include <net/ethernet.h>			// for ether_header
#include <net/if.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>

#include "ns_arp.h"
#include "ns_arp_packet.h"

// RETurn ON FAILure
#define RETONFAIL(x) { int a = x; if(a) return a; }

// FAILure ON ARGumentS
#define FAILONARGS(i, max) { if(max==i+1) { \
				fprintf(stderr, "Invalid number of arguments!\n"); \
				return -1; } }

const char *USAGE_INFO = \
"This program is a daemon wakes up a device on the local\n"
"network based upon if the local system tries to access it\n"
"via LAN network.\n"
"Usage:\n"
"\t-h/--help - this screen\n"
"\t-i - IP address of device to wake up\n"
"\t-m - MAC (hardware) address of device to wake up\n"
"\t-d - network device to check traffic from (eg. eth0)\n"
"\t-b - broadcast IP address (eg. 192.168.1.255)\n";

void cleanup();
void create_magic_packet();
void sig_handler(int);
int initialize();
int watch_packets();
int process_packet(unsigned char* , int);
int parse_arp(unsigned char *);
int parse_ethhdr(unsigned char*, int);
int get_local_ip();
int send_magic_packet();

struct main {
	char *eth_dev_s;
	char *eth_ip_s;
	char *dev_ip_s;
	char *dev_mac_s;
	char *broadcast_ip_s;

	unsigned char eth_ip[4];
	unsigned char dev_ip[4];
	unsigned char dev_mac[6];

	unsigned char *buffer;
	unsigned char *magic_packet;
	int sock_raw;
	bool alive;
} m;

void cleanup() {
	close(m.sock_raw);
	free(m.buffer);
	free(m.magic_packet);
}

// handle signals, such as CTRL-C
void sig_handler(int signo) {
	m.alive = false;
}

int initialize() {
	create_magic_packet();

	RETONFAIL(get_local_ip());

	// attach signal handler
	struct sigaction action;
	memset(&action, 0, sizeof(action));
	action.sa_handler = &sig_handler;
	sigaction(SIGINT, &action, NULL);  // close by CTRL-C
	sigaction(SIGTERM, &action, NULL); // close by task manager and/or kill

	// set alive flag
	m.alive = true;

	// allocate memory for storing packets
	m.buffer = (unsigned char *) malloc(65536);

	// open the socket
	m.sock_raw = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL)) ;

	// listen on a specific network device
	setsockopt(m.sock_raw, SOL_SOCKET, SO_BINDTODEVICE, m.eth_dev_s, strlen(m.eth_dev_s)+1);

	if(m.sock_raw < 0) {
		perror("socket error");
		return 1;
	}
	return 0;
}

int watch_packets() {
	int saddr_size, data_size;
	struct sockaddr saddr;

	while(m.alive) {
		saddr_size = sizeof saddr;
		// receive a packet
		data_size = recvfrom(m.sock_raw, m.buffer, 65536, 0, &saddr, (socklen_t*)&saddr_size);
		if(data_size < 0) {
			printf("recvfrom error, failed to get packets\n");
			return 1;
		}
		// now process the packet
		RETONFAIL(process_packet(m.buffer, data_size));
	}
	return 0;
}

int process_packet(unsigned char* buffer, int size) {
	// get the IP Header part of this packet, excluding the ethernet header
	//struct iphdr *iph = (struct iphdr*)(buffer + sizeof(struct ethhdr));
	
	// TODO: Research packet types for ARP
	// Known types are: 157 (from router), 87 and 129
	//printf("%u\n", iph->protocol);

	// for now, accept all packets
	RETONFAIL(parse_ethhdr(buffer, size));

	return 0;
}

int parse_arp(unsigned char *data) {
	ns_arp_packet_hdr_t *arp_hdr = (ns_arp_packet_hdr_t *) data;
	ns_arp_IPv4_eth_packet_t *arp_IPv4 = NULL;

	if(ntohs(arp_hdr->ns_arp_hw_type) != NS_ARP_ETHERNET_TYPE) {
		fprintf(stderr, "dis not ethernet :(\n");
		exit(EXIT_FAILURE);
	}

	if(ntohs(arp_hdr->ns_arp_proto_type) != NS_ETH_TYPE_IPv4) {
		fprintf(stderr, "i bet you're using IPv4\n");
		exit(EXIT_FAILURE);
	}

	arp_IPv4 = (ns_arp_IPv4_eth_packet_t *) data;

	// sender and target address
	unsigned char *sa = arp_IPv4->ns_arp_sender_proto_addr;
	unsigned char *ta = arp_IPv4->ns_arp_target_proto_addr;

	// sender and target hardware
	//unsigned char *sh = arp_IPv4->ns_arp_sender_hw_addr;
	//unsigned char *th = arp_IPv4->ns_arp_sender_hw_addr;

	// ARP type
	uint16_t type = ntohs(arp_IPv4->ns_arp_hdr.ns_arp_opcode);

	if(type == NS_ARP_REQUEST) {
		// if source matches to host
		// and if target matches send magic
		if(!memcmp(m.eth_ip, sa, 4*sizeof(unsigned char))) {
			if(!memcmp(m.dev_ip, ta, 4*sizeof(unsigned char))) {
				RETONFAIL(send_magic_packet());
				#ifdef DEBUG
					puts("Sent magic packet!");
				#endif
			}
		}
	}
	return 0;
}

int parse_ethhdr(unsigned char* buffer, int size) {
	struct ethhdr *eth = (struct ethhdr *)buffer;
	
	// convert network-endianess to native endianess
	unsigned short eth_protocol = ntohs(eth->h_proto);

	if(eth_protocol == 0x0806) {
		unsigned char* arphdr = buffer + sizeof(struct ethhdr);
		RETONFAIL(parse_arp(arphdr));
	}
	return 0;
}

int read_args(int argc, char *argv[]) {
	m.dev_ip_s = NULL;
	m.dev_mac_s = NULL;
	m.eth_dev_s = NULL;
	m.broadcast_ip_s = NULL;

	for(int i=1; i<argc; i++) {
		if(!strcmp(argv[i], "-h")||!strcmp(argv[i], "--help")) {
			puts(USAGE_INFO);
			return 0;
		} else if(!strcmp(argv[i], "-i")) {
			FAILONARGS(i, argc);
			m.dev_ip_s = argv[i+1];
			i++;
		} else if(!strcmp(argv[i], "-m")) {
			FAILONARGS(i, argc);
			m.dev_mac_s = argv[i+1];
			i++;
		} else if(!strcmp(argv[i], "-d")) {
			FAILONARGS(i, argc);
			m.eth_dev_s = argv[i+1];
			i++;
		} else if(!strcmp(argv[i], "-b")) {
			FAILONARGS(i, argc);
			m.broadcast_ip_s = argv[i+1];
			i++;
		}
	}
	return 0;
}

int parse_args() {
	if(!m.dev_ip_s) {
		fprintf(stderr, "IP address of device to wake up not specified!\n");
		return 1;
	}
	if(!m.dev_mac_s) {
		fprintf(stderr, "MAC (hardware) address of device to wake up not specified!\n");
		return 1;
	}
	if(!m.eth_dev_s) {
		fprintf(stderr, "Ethernet device to record traffic from not specified!\n");
		return 1;
	}
	if(!m.broadcast_ip_s) {
		fprintf(stderr, "Broadcast IP not specified!\n");
		return 1;
	}

	int error = sscanf(m.dev_mac_s, "%2hhx:%2hhx:%2hhx:%2hhx:%2hhx:%2hhx", &m.dev_mac[0], &m.dev_mac[1],
									&m.dev_mac[2], &m.dev_mac[3], &m.dev_mac[4], &m.dev_mac[5]);
	// maybe the user typed it uppercase?
	if(error != 6) {
		error = sscanf(m.dev_mac_s, "%2hhX:%2hhX:%2hhX:%2hhX:%2hhX:%2hhX", &m.dev_mac[0], &m.dev_mac[1],
								&m.dev_mac[2], &m.dev_mac[3], &m.dev_mac[4], &m.dev_mac[5]);
	}
	if(error != 6) {
		fprintf(stderr, "Invalid MAC address: \"%s\"\n", m.dev_mac_s);
		return 1;
	}
	error = sscanf(m.dev_ip_s, "%hhu.%hhu.%hhu.%hhu", &m.dev_ip[0],
									&m.dev_ip[1], &m.dev_ip[2], &m.dev_ip[3]);
	if(error != 4) {
		fprintf(stderr, "Invalid IP address: \"%s\"", m.dev_ip_s);
	}
	return 0;
}

void create_magic_packet() {
	m.magic_packet = malloc(102*sizeof(unsigned char));

	// 6 x 0xFF on start of packet
	memset(m.magic_packet, 0xFF, 6);

	// rest are just copies of the MAC address
	for(int i = 1; i <= 16; i++) {
		memcpy(&m.magic_packet[i*6], &m.dev_mac, 6*sizeof(unsigned char));
	}
}

int send_magic_packet() {
	int udpSocket = 1;
	int broadcast = 1;
	struct sockaddr_in udpClient, udpServer;

	// setup broadcast socket
	udpSocket = socket(AF_INET, SOCK_DGRAM, 0);
	if(setsockopt(udpSocket, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast)) == -1) {
		perror("socket error");
		return 1;
	}

	// set parameters
	udpClient.sin_family = AF_INET;
	udpClient.sin_addr.s_addr = INADDR_ANY;
	udpClient.sin_port = 0;

	// bind socket
	bind(udpSocket, (struct sockaddr*) &udpClient, sizeof(udpClient));

	// set server end point (the broadcast address)
	udpServer.sin_family = AF_INET;
	udpServer.sin_addr.s_addr = inet_addr(m.broadcast_ip_s);
	udpServer.sin_port = htons(9);

	// set server end point
	sendto(udpSocket, m.magic_packet, sizeof(unsigned char)*102, 0, (struct sockaddr*) &udpServer, sizeof(udpServer));

	// clean after use
	close(udpSocket);

	return 0;
}

int get_local_ip() {
	int fd;
	struct ifreq ifr;

	// open socket
	fd = socket(AF_INET, SOCK_DGRAM, 0);

	if(fd < 0) {
		perror("socket error");
		return 1;
	}

	// get a IPv4 address specifically
	ifr.ifr_addr.sa_family = AF_INET;

	// get address for the following network device
	strncpy(ifr.ifr_name, m.eth_dev_s, IFNAMSIZ-1);

	// go fetch
	int error = ioctl(fd, SIOCGIFADDR, &ifr);
	if(error == -1) {
		perror("ioctl error");
		return 1;
	}

	// clean up
	close(fd);

	// get the darn address
	m.eth_ip_s = inet_ntoa(((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr);

	// convert IP back to binary
	sscanf(m.eth_ip_s, "%hhu.%hhu.%hhu.%hhu", &m.eth_ip[0],
						&m.eth_ip[1], &m.eth_ip[2], &m.eth_ip[3]);
	return 0;
}

int main(int argc, char *argv[]) {
	RETONFAIL(read_args(argc, argv));
	RETONFAIL(parse_args());
	RETONFAIL(initialize());
	RETONFAIL(watch_packets());
	cleanup();
	return 0;
}

