/* dhcpd.c
 *
 * udhcp DHCP client
 *
 * Russ Dill <Russ.Dill@asu.edu> July 2001
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
 
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/file.h>
#include <unistd.h>
#include <getopt.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <time.h>
#include <string.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <errno.h>

#include "dhcpd.h"
#include "dhcpc.h"
#include "options.h"
#include "clientpacket.h"
#include "packet.h"
#include "script.h"
#include "socket.h"
#include "debug.h"

static int state;
static unsigned long requested_ip; /* = 0 */
static unsigned long server_addr;
static unsigned long timeout;
static int packet_num; /* = 0 */
static int pid_fd;

#define LISTEN_NONE 0
#define LISTEN_KERNEL 1
#define LISTEN_RAW 2
static int listen_mode = LISTEN_RAW;

#define DEFAULT_SCRIPT	"/usr/share/udhcpc/default.script"

struct client_config_t client_config = {
	/* Default options. */
	abort_if_no_lease: 0,
	foreground: 0,
	interface: "eth0",
	pidfile: NULL,
	script: DEFAULT_SCRIPT,
	clientid: NULL,
	hostname: NULL,
	ifindex: 0,
	arp: "\0\0\0\0\0\0",		/* appease gcc-3.0 */
};

static void print_usage(void)
{
	printf(
"Usage: udhcpcd [OPTIONS]\n\n"
"  -c, --clientid=CLIENTID         Client identifier\n"
"  -H, --hostname=HOSTNAME         Client hostname\n"
"  -f, --foreground                Do not fork after getting lease\n"
"  -i, --interface=INTERFACE       Interface to use (default: eth0)\n"
"  -n, --now                       Exit with failure if lease cannot be\n"
"                                  immediately negotiated.\n"
"  -p, --pidfile=file              Store process ID of daemon in file\n"
"  -r, --request=IP                IP address to request (default: none)\n"
"  -s, --script=file               Run file at dhcp events (default:\n"
"                                  " DEFAULT_SCRIPT ")\n"
"  -v, --version                   Display version\n"
	);
}


/* SIGUSR1 handler (renew) */
static void renew_requested(int sig)
{
	sig = 0;
	LOG(LOG_INFO, "Received SIGUSR1");
	if (state == BOUND || state == RENEWING || state == REBINDING ||
	    state == RELEASED) {
	    	listen_mode = LISTEN_KERNEL;
		server_addr = 0;
		packet_num = 0;
		state = RENEW_REQUESTED;
	}

	if (state == RELEASED) {
		listen_mode = LISTEN_RAW;
		state = INIT_SELECTING;
	}

	/* Kill any timeouts because the user wants this to hurry along */
	timeout = 0;
}


/* SIGUSR2 handler (release) */
static void release_requested(int sig)
{
	sig = 0;
	LOG(LOG_INFO, "Received SIGUSR2");
	/* send release packet */
	if (state == BOUND || state == RENEWING || state == REBINDING) {
		send_release(server_addr, requested_ip); /* unicast */
		run_script(NULL, "deconfig");
	}

	listen_mode = 0;
	state = RELEASED;
	timeout = 0xffffffff;
}


static void pidfile_acquire(void)
{
	if (client_config.pidfile == NULL) return;

	pid_fd = open(client_config.pidfile, O_CREAT | O_WRONLY, 0644);
	if (pid_fd < 0) {
		LOG(LOG_ERR, "Unable to open pidfile %s: %s\n",
		    client_config.pidfile, strerror(errno));
	} else {
		lockf(pid_fd, F_LOCK, 0);
	}
}


static void pidfile_write_release(void)
{
	FILE *out;

	if (client_config.pidfile == NULL || pid_fd < 0) return;

	if ((out = fdopen(pid_fd, "w")) != NULL) {
		fprintf(out, "%d\n", getpid());
		fclose(out);
	}
	lockf(pid_fd, F_UNLCK, 0);
	close(pid_fd);
}


static void background(void)
{
	if (!client_config.foreground) {
		pidfile_acquire(); /* hold lock during fork. */
		switch(fork()) {
		case -1:
			perror("fork");
			exit(1);
			/*NOTREACHED*/
		case 0:
			break; /* child continues */
		default:
			exit(0); /* parent exits */
			/*NOTREACHED*/
		}
		close(0);
		close(1);
		close(2);
		setsid();
		client_config.foreground = 1; /* Do not fork again. */
		pidfile_write_release();
	}
}


#ifdef COMBINED_BINARY
int udhcpc(int argc, char *argv[])
#else
int main(int argc, char *argv[])
#endif
{
	char *temp, *message;
	unsigned long t1 = 0, t2 = 0, xid = 0;
	unsigned long start = 0, lease;
	fd_set rfds;
	int fd, retval;
	struct timeval tv;
	int c, len;
	struct ifreq ifr;
	struct dhcpMessage packet;
	struct in_addr temp_addr;

	static struct option options[] = {
		{"clientid",	required_argument,	0, 'c'},
		{"foreground",	no_argument,		0, 'f'},
		{"hostname",	required_argument,	0, 'H'},
		{"help",	no_argument,		0, 'h'},
		{"interface",	required_argument,	0, 'i'},
		{"now", 	no_argument,		0, 'n'},
		{"pidfile",	required_argument,	0, 'p'},
		{"request",	required_argument,	0, 'r'},
		{"script",	required_argument,	0, 's'},
		{"version",	no_argument,		0, 'v'},
		{0, 0, 0, 0}
	};

	/* get options */
	while (1) {
		int option_index = 0;
		c = getopt_long(argc, argv, "c:fH:hi:np:r:s:v", options, &option_index);
		if (c == -1) break;
		
		switch (c) {
		case 'c':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.clientid) free(client_config.clientid);
			client_config.clientid = malloc(len + 2);
			client_config.clientid[OPT_CODE] = DHCP_CLIENT_ID;
			client_config.clientid[OPT_LEN] = len;
			strncpy(client_config.clientid + 2, optarg, len);
			break;
		case 'f':
			client_config.foreground = 1;
			break;
		case 'H':
			len = strlen(optarg) > 255 ? 255 : strlen(optarg);
			if (client_config.hostname) free(client_config.hostname);
			client_config.hostname = malloc(len + 2);
			client_config.hostname[OPT_CODE] = DHCP_HOST_NAME;
			client_config.hostname[OPT_LEN] = len;
			strncpy(client_config.hostname + 2, optarg, len);
			break;
		case 'h':
			print_usage();
			return 0;
		case 'i':
			client_config.interface =  optarg;
			break;
		case 'n':
			client_config.abort_if_no_lease = 1;
			break;
		case 'p':
			client_config.pidfile = optarg;
			break;
		case 'r':
			requested_ip = inet_addr(optarg);
			break;
		case 's':
			client_config.script = optarg;
			break;
		case 'v':
			printf("udhcpcd, version %s\n\n", VERSION);
			break;
		}
	}

	OPEN_LOG("udhcpc");
	LOG(LOG_INFO, "Moreton Bay DHCP Client (v%s) started", VERSION);

	pidfile_acquire();
	pidfile_write_release();

	if ((fd = socket(AF_INET, SOCK_RAW, IPPROTO_RAW)) >= 0) {
		strcpy(ifr.ifr_name, client_config.interface);
		if (ioctl(fd, SIOCGIFINDEX, &ifr) == 0) {
			DEBUG(LOG_INFO, "adapter index %d", ifr.ifr_ifindex);
			client_config.ifindex = ifr.ifr_ifindex;
		} else {
			LOG(LOG_ERR, "SIOCGIFINDEX failed! %s", sys_errlist[errno]);
			return 1;
		}
		if (ioctl(fd, SIOCGIFHWADDR, &ifr) == 0) {
			memcpy(client_config.arp, ifr.ifr_hwaddr.sa_data, 6);
			DEBUG(LOG_INFO, "adapter hardware address %02x:%02x:%02x:%02x:%02x:%02x",
				client_config.arp[0], client_config.arp[1], client_config.arp[2], 
				client_config.arp[3], client_config.arp[4], client_config.arp[5]);
		} else {
			LOG(LOG_ERR, "SIOCGIFHWADDR failed! %s", sys_errlist[errno]);
			return 1;
		}
	} else {
		LOG(LOG_ERR, "socket failed! %s", sys_errlist[errno]);
		return 1;
	}
	close(fd);
	fd = -1;

	/* setup signal handlers */
	signal(SIGUSR1, renew_requested);
	signal(SIGUSR2, release_requested);
	
	state = INIT_SELECTING;
	run_script(NULL, "deconfig");

	for (;;) {
		if (fd > 0) {
			close(fd);
			fd = -1;
		}
		
		if (listen_mode == LISTEN_KERNEL) {
			if ((fd = listen_socket(INADDR_ANY, CLIENT_PORT, client_config.interface)) < 0) {
				LOG(LOG_ERR, "couldn't create server socket -- au revoir");
				exit(0);
			}			
		} else if (listen_mode == LISTEN_RAW) {
			if ((fd = raw_socket(client_config.interface)) < 0) {
				LOG(LOG_ERR, "couldn't create raw socket -- au revoir");
				exit(0);
			}			
		} else fd = -1;

		tv.tv_sec = timeout - time(0);
		tv.tv_usec = 0;
		FD_ZERO(&rfds);
		if (listen_mode) FD_SET(fd, &rfds);
		
		if (tv.tv_sec > 0) {
			retval = select(fd + 1, &rfds, NULL, NULL, &tv);
		} else retval = 0; /* If we already timed out, fall through */
		
		if (retval == 0) {
			/* timeout dropped to zero */
			switch (state) {
			case INIT_SELECTING:
				if (packet_num < 3) {
					if (packet_num == 0)
						xid = random_xid();

					/* send discover packet */
					send_discover(xid, requested_ip); /* broadcast */
					
					timeout = time(0) + ((packet_num == 2) ? 10 : 2);
					packet_num++;
				} else {
					if (client_config.abort_if_no_lease) {
						LOG(LOG_INFO,
						    "No lease, failing.");
						exit(1);
				  	}
					/* wait to try again */
					packet_num = 0;
					timeout = time(0) + 60;
				}
				break;
			case RENEW_REQUESTED:
			case REQUESTING:
				if (packet_num < 3) {
					/* send request packet */
					if (state == RENEW_REQUESTED)
						send_renew(xid, server_addr, requested_ip); /* unicast */
					else send_selecting(xid, server_addr, requested_ip); /* broadcast */
					
					timeout = time(0) + ((packet_num == 2) ? 10 : 2);
					packet_num++;
				} else {
					/* timed out, go back to init state */
					state = INIT_SELECTING;
					timeout = time(0);
					packet_num = 0;
					listen_mode = LISTEN_RAW;
					
				}
				break;
			case BOUND:
				/* Lease is starting to run out, time to enter renewing state */
				state = RENEWING;
				listen_mode = LISTEN_KERNEL;
				DEBUG(LOG_INFO, "Entering renew state");
				/* fall right through */
			case RENEWING:
				/* Either set a new T1, or enter REBINDING state */
				if ((t2 - t1) <= (lease / 14400 + 1)) {
					/* timed out, enter rebinding state */
					state = REBINDING;
					timeout = time(0) + (t2 - t1);
					DEBUG(LOG_INFO, "Entering rebinding state");
				} else {
					/* send a request packet */
					send_renew(xid, server_addr, requested_ip); /* unicast */
					
					t1 = (t2 - t1) / 2 + t1;
					timeout = t1 + start;
				}
				break;
			case REBINDING:
				/* Either set a new T2, or enter INIT state */
				if ((lease - t2) <= (lease / 14400 + 1)) {
					/* timed out, enter init state */
					state = INIT_SELECTING;
					LOG(LOG_INFO, "Lease lost, entering init state");
					run_script(NULL, "deconfig");
					timeout = time(0);
					packet_num = 0;
					listen_mode = LISTEN_RAW;
				} else {
					/* send a request packet */
					send_renew(xid, 0, requested_ip); /* broadcast */

					t2 = (lease - t2) / 2 + t2;
					timeout = t2 + start;
				}
				break;
			case RELEASED:
				/* yah, I know, *you* say it would never happen */
				timeout = 0xffffffff;
				break;
			}
		} else if (listen_mode != LISTEN_NONE && FD_ISSET(fd, &rfds)) {
			/* a packet is ready, read it */
			
			if (listen_mode == LISTEN_KERNEL) {
				if (get_packet(&packet, fd) < 0) continue;
			} else {
				if (get_raw_packet(&packet, fd) < 0) continue;
			} 
			
			if (packet.xid != xid) {
				DEBUG(LOG_INFO, "Ignoring XID %lx (our xid is %lx)",
					(unsigned long) packet.xid, xid);
				continue;
			}
			
			if ((message = get_option(&packet, DHCP_MESSAGE_TYPE)) == NULL) {
				DEBUG(LOG_ERR, "couldnt get option from packet -- ignoring");
				continue;
			}
			
			switch (state) {
			case INIT_SELECTING:
				/* Must be a DHCPOFFER to one of our xid's */
				if (*message == DHCPOFFER) {
					if ((temp = get_option(&packet, DHCP_SERVER_ID))) {
						memcpy(&server_addr, temp, 4);
						xid = packet.xid;
						requested_ip = packet.yiaddr;
						
						/* enter requesting state */
						state = REQUESTING;
						timeout = time(0);
						packet_num = 0;
					} else {
						DEBUG(LOG_ERR, "No server ID in message");
					}
				}
				break;
			case RENEW_REQUESTED:
			case REQUESTING:
			case RENEWING:
			case REBINDING:
				if (*message == DHCPACK) {
					if (!(temp = get_option(&packet, DHCP_LEASE_TIME))) {
						LOG(LOG_ERR, "No lease time with ACK, using 1 hour lease");
						lease = 60*60;
					} else {
						memcpy(&lease, temp, 4);
						lease = ntohl(lease);
					}
						
					/* enter bound state */
					t1 = lease / 2;
					
					/* little fixed point for n * .875 */
					t2 = (lease * 0x7) >> 3;
					temp_addr.s_addr = packet.yiaddr;
					LOG(LOG_INFO, "Lease of %s obtained, lease time %ld", 
						inet_ntoa(temp_addr), lease);
					start = time(0);
					timeout = t1 + start;
					requested_ip = packet.yiaddr;
					run_script(&packet,
						   ((state == RENEWING || state == REBINDING) ? "renew" : "bound"));

					state = BOUND;
					listen_mode = LISTEN_NONE;
					background();
					
				} else if (*message == DHCPNAK) {
					/* return to init state */
					LOG(LOG_INFO, "Received DHCP NAK");
					if (state != REQUESTING)
						run_script(NULL, "deconfig");
					state = INIT_SELECTING;
					timeout = time(0);
					requested_ip = 0;
					packet_num = 0;
					listen_mode = LISTEN_RAW;
				}
				break;
			case BOUND:
			case RELEASED:
				/* ignore all packets */
				break;
			}					
		} else if (retval == -1 && errno == EINTR) {
			/* a signal was caught */
			
		} else {
			/* An error occured */
			DEBUG(LOG_ERR, "Error on select");
		}
		
	}
	return 0;
}

