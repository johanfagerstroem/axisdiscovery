#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/poll.h>
#include <sys/types.h>
#include <netdb.h>
#include <arpa/inet.h>

#define SSDP_ADDRESS "255.255.255.255"
#define SSDP_PORT 1900
#define SSDP_TIMEOUT 1

/* Axis SSDP and rootdesc are normally around 500 and 1500 bytes respectively */
#define AXIS_SSDP_BUFLEN 512
#define AXIS_ROOTDESC_BUFLEN 2000

#define AXIS_SSDP_ST "urn:axis-com:service:*"

const char *REQUEST_AXIS = "M-SEARCH * HTTP/1.1\r\n\
HOST: 239.255.255.250:1900\r\n\
MAN: \"ssdp:discover\"\r\n\
MX: 2\r\n\
ST: " AXIS_SSDP_ST "\r\n\
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
  begin += 17;

  /* Strip _occasional_ last slash */
  if (*(end - 1) == '/') { end-=1; }

  dev->url = strndup(begin, end - begin);

  return dev;
}

int device_compare(device *dev1, device *dev2) {
  int modelcmp = strcmp(dev1->model, dev2->model);
  return modelcmp == 0 ? strcmp(dev1->mac, dev2->mac) : modelcmp;
}

void device_print(device *dev)
{
  printf("%-15s  %-10s  %s\n", dev->model, dev->mac, dev->url);
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
    while (next && device_compare(next->device, dev) < 0) {
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

  int rootdesc_reserved = AXIS_ROOTDESC_BUFLEN;
  char *rootdesc = calloc(rootdesc_reserved, 1);
  int rootdesc_read = 0;
  int res;

  while((res = read(fd, rootdesc + rootdesc_read, rootdesc_reserved-rootdesc_read - 1)) > 0){
    rootdesc_read += res;
    /* If we filled the allocated memory, extended it to twice the size */
    if (rootdesc_read == rootdesc_reserved-1) {
      rootdesc_reserved += AXIS_ROOTDESC_BUFLEN;
      if (!realloc(rootdesc, rootdesc_reserved)) {
        perror("get_rootdesc realloc() failed");
        free(rootdesc);
        return NULL;
      }
      bzero(rootdesc + rootdesc_read, rootdesc_reserved - rootdesc_read);
    }
  }

  if (res == -1 ) {
    perror("get_rootdesc read() failed");
    free(rootdesc);
    return NULL;
  }

  if (close(fd) == -1) {
    perror("get_rootdesc close() failed");
    free(rootdesc);
    return NULL;
  }

  return rootdesc;
}

/* Some devices (read: Philips Hue) do not respect the ST specified
 * in the SSDP request */
int is_axis_response(char *ssdp) {
  return strstr(ssdp, AXIS_SSDP_ST) == NULL ? 0 : 1;
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

  if (sendto
      (fd, REQUEST_AXIS, strlen(REQUEST_AXIS), 0,
       (struct sockaddr *)&addr, slen) == -1) {
    perror("send_ssdp sendto() failed");
    exit(1);
  }

  struct sockaddr_in src_addr;
  socklen_t src_slen = sizeof(src_addr);
  char ssdp[AXIS_SSDP_BUFLEN];
  int pollres;
  struct pollfd fds[1];
  fds[0].fd = fd;
  fds[0].events = POLLIN;
  time_t deadline = time(0) + SSDP_TIMEOUT;
  time_t wait;

  while (1) {
    wait = deadline - time(0);
    if (wait < 0) break;

    pollres = poll(fds, 1, wait*1000);
    if (pollres == -1) {
      perror ("send_ssdp poll() failed");
      exit(1);
    } else if (pollres == 0) {
      /* Timeout */
      break;
    }

    if (fds[0].revents & POLLIN) {
      bzero(ssdp, AXIS_SSDP_BUFLEN);
      if (recvfrom
        (fd, ssdp, AXIS_SSDP_BUFLEN - 1, 0,
	   (struct sockaddr *)&src_addr, &src_slen) == -1) {
        perror("send_ssdp recvfrom() failed");
        exit(1);
      }

      if (!is_axis_response(ssdp)) {
        continue;
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
