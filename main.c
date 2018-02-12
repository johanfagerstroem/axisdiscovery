#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>

#define SSDP_ADDRESS "239.255.255.250"
#define SSDP_PORT 1900
#define SSDP_TIMEOUT 1
#define AXIS_SSDP_BUFLEN 512	//Max length of response buffer. AXIS SSDP is normally 496.
#define AXIS_ROOTDESC_BUFLEN 2000	//Should hold everything

const char *REQUEST_AXIS = "M-SEARCH * HTTP/1.1\r\n\
HOST: 239.255.255.250:1900\r\n\
MAN: \"ssdp:discover\"\r\n\
MX: 2\r\n\
ST: urn:axis-com:service:BasicService:1\r\n\
\r\n";

typedef struct device {
  char *mac, *model, *url;
} device;

void device_free(device *dev)
{
  if (dev->mac) {
    free(dev->mac);
    dev->mac = NULL;
  }
  if (dev->model) {
    free(dev->model);
    dev->model = NULL;
  }
  if (dev->url) {
    free(dev->url);
    dev->url = NULL;
  }
  free(dev);
}

device *device_new_from_rootdesc(char *rootdesc)
{
  device *dev = (device *)malloc(sizeof(device));
  dev->mac = dev->model = dev->url = NULL;
  char *begin;
  char *end;

  begin = strstr(rootdesc, "<serialNumber>");
  end = begin ? strstr(begin, "</serialNumber>") : NULL;
  if (end == NULL) {
    device_free(dev);
    return NULL;
  }
  begin += 14;
  dev->mac = strndup(begin, end - begin);

  begin = strstr(rootdesc, "<modelNumber>");
  end = begin ? strstr(begin, "</modelNumber>") : NULL;
  if (end == NULL) {
    device_free(dev);
    return NULL;
  }
  begin += 13;
  dev->model = strndup(begin, end - begin);

  begin = strstr(rootdesc, "<presentationURL>");
  end = begin ? strstr(begin, "</presentationURL>") : NULL;
  if (end == NULL) {
    device_free(dev);
    return NULL;
  }
  // Strip <PresentationURL>http://
  begin += 24;

  // Remove port from the presentation url.
  // Strip _occasional_ last slash.
  if (*(end - 1) == '/') { end-=4; }
  else { end-=3;}

  dev->url = strndup(begin, end - begin);

  return dev;
}

void device_print(device *dev)
{
  printf("%-15s  %-15s  %s\n", dev->model, dev->url, dev->mac);
}

typedef struct listnode {
  struct device *device;
  struct listnode *ptr;
} listnode;

listnode *devicelist = NULL;

void devicelist_insert(device *dev)
{
  listnode *temp, *prev, *next;
  temp = (listnode *) malloc(sizeof(listnode));
  temp->device = dev;
  temp->ptr = NULL;
  if (!devicelist) {
    devicelist = temp;
  } else {
    prev = NULL;
    next = devicelist;
    while (next && strcmp(next->device->model, dev->model) < 0) {
      prev = next;
      next = next->ptr;
    }
    if (!next) {
      prev->ptr = temp;
    } else {
      if (prev) {
	temp->ptr = prev->ptr;
	prev->ptr = temp;
      } else {
	temp->ptr = devicelist;
	devicelist = temp;
      }
    }
  }
}

void devicelist_print_and_destroy()
{
  listnode *previous = devicelist;
  listnode *current = devicelist;
  while (current) {
    device_print(current->device);
    previous = current;
    current = previous->ptr;
    device_free(previous->device);
    free(previous);
  }
}

int ssdp_parse_location_port(char *ssdp) {
  char *begin, *end;

  begin = strstr(ssdp, "LOCATION: http://");
  if (begin == NULL) return -1;
  begin += 17;

  begin = strstr(begin, ":");
  if (begin == NULL) return -1;
  begin += 1;

  end = strstr(begin, "/");
  if (end == NULL) return -1;

  char port[6];
  bzero(port, 6);
  strncpy(port, begin, end-begin);
  int portnbr = atoi(port);
  return portnbr ? portnbr : -1;
}

char *ssdp_parse_location_resource(char *ssdp) {
  char *begin, *end;

  begin = strstr(ssdp, "LOCATION: http://");
  if (begin == NULL) return NULL;
  begin += 17;

  begin = strstr(begin, "/");
  if (begin == NULL) return NULL;

  end = strstr(begin, ".xml");
  if (end == NULL) return NULL;
  end += 4;

  return strndup(begin, end-begin);
}

char *get_rootdesc(char *address, int port, char *resource)
{
  struct sockaddr_in addr = { 0 };
  int on = 1;

  addr.sin_port = htons(port);
  addr.sin_family = AF_INET;

  if (inet_aton(address, &addr.sin_addr) == 0) {
    fprintf(stderr, "get_rootdesc inet_aton() failed");
    return NULL;
  }

  int fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (fd == -1) {
    perror("get_rootdesc socket() failed");
    return NULL;
  }

  if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&on,
		 sizeof(int)) == -1) {
    perror("get_rootdesc setsockopt(tcp_nodelay) failed");
    return NULL;
  }

  if (connect(fd, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) == -1) {
    perror("get_rootdesc connect() failed");
    return NULL;

  }

  char request[17 + strlen(resource)];
  sprintf(request, "GET %s HTTP/1.1\r\n\r\n", resource);
  if (write(fd, request, strlen(request)) == -1) {
    perror("get_rootdesc write() failed");
    return NULL;
  }

  if (shutdown(fd, SHUT_WR) == -1) {
    perror("get_rootdesc shutdown() failed");
    return NULL;
  }

  char *rootdesc = calloc(AXIS_ROOTDESC_BUFLEN, 1);
  if (recv(fd, rootdesc, AXIS_ROOTDESC_BUFLEN - 1, MSG_WAITALL) == -1) {
    perror("get_rootdesc recvfrom() failed");
    return NULL;
  }

  if (close(fd) == -1) {
    perror("get_rootdesc close() failed");
    free(rootdesc);
    return NULL;
  }

  return rootdesc;
}

void send_ssdp_and_populate_device_list(char *address)
{
  struct sockaddr_in addr = { 0 };
  int slen = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(SSDP_PORT);

  if (inet_aton(address, &addr.sin_addr) == 0) {
    printf("Error: Malformed broadcast address\n");
    exit(1);
  }

  int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (fd == -1) {
    perror("send_ssdp socket() failed");
    exit(1);
  }

  int broadcast = 1;
  if (setsockopt
      (fd, SOL_SOCKET, SO_BROADCAST, (void *)&broadcast,
       sizeof(broadcast)) == -1) {
    perror("send_ssdp setsockopt(broadcast) failed");
    exit(1);
  }

  struct timeval tv;
  tv.tv_usec = 0;
  tv.tv_sec = SSDP_TIMEOUT;
  if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1) {
    perror("send_ssdp setsockopt(timeout) failed");
    exit(1);
  }

  if (sendto
      (fd, REQUEST_AXIS, strlen(REQUEST_AXIS), 0,
       (struct sockaddr *)&addr, slen) == -1) {
    perror("send_ssdp sendto() failed");
    exit(1);
  }

  struct sockaddr_in src_addr;
  socklen_t src_slen = sizeof(src_addr);
  char ssdp[AXIS_SSDP_BUFLEN];

  while (1) {
    bzero(ssdp, AXIS_SSDP_BUFLEN);
    if (recvfrom
	(fd, ssdp, AXIS_SSDP_BUFLEN - 1, MSG_WAITALL,
	 (struct sockaddr *)&src_addr, &src_slen) == -1) {
      if (errno == EAGAIN) {
	/* Expected timeout, just break the loop */
	break;
      } else {
	perror("recvfrom() failed");
	exit(1);
      }
    }

    int port = ssdp_parse_location_port(ssdp);
    char *resource = ssdp_parse_location_resource(ssdp);
    if (port == -1 || resource == NULL) {
      continue;
    }

    char *rootdesc = get_rootdesc(inet_ntoa(src_addr.sin_addr), port, resource);
    free(resource);
    if (rootdesc == NULL) {
      continue;
    }

    device *dev = device_new_from_rootdesc(rootdesc);
    free(rootdesc);
    if (dev == NULL) {
      continue;
    }

    devicelist_insert(dev);

  }

  if (close(fd) == -1) {
    perror("send_ssdp close() failed");
    exit(1);
  }
}

int main(int argc, char *argv[])
{
  if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
    printf("Usage: %s <optional broadcast address>\n", argv[0]);
    exit(0);
  }

  if (argc > 1 && (!strcmp(argv[1], "--version") || !strcmp(argv[1], "-v"))) {
    printf("%s\n", VERSION);
    exit(0);
  }

  char *broadcast_address = argc > 1 ? argv[1] : SSDP_ADDRESS;

  send_ssdp_and_populate_device_list(broadcast_address);
  devicelist_print_and_destroy();

  return 0;
}
