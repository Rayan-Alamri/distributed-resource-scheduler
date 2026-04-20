#ifndef PROTOCOL_H
#define PROTOCOL_H

#include "models.h"
#include <sys/types.h>

/* Blocking recv until exactly len bytes are read. Returns 0 on success, -1 on error/EOF. */
int recv_full(int fd, void *buf, size_t len);

/* Blocking send until exactly len bytes are written. Returns 0 on success, -1 on error. */
int send_full(int fd, const void *buf, size_t len);

/* Convert all uint32 fields of a NetworkPayload to network byte order (host → network). */
void payload_to_net(NetworkPayload *p);

/* Convert all uint32 fields of a NetworkPayload to host byte order (network → host). */
void payload_to_host(NetworkPayload *p);

#endif /* PROTOCOL_H */
