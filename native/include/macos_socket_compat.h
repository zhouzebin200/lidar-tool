#pragma once
#include <sys/socket.h>
#include <time.h>
#include <errno.h>

#if defined(__APPLE__)
#include <libkern/OSByteOrder.h>
#ifndef htobe16
#define htobe16(x) OSSwapHostToBigInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define le16toh(x) OSSwapLittleToHostInt16(x)

#define htobe32(x) OSSwapHostToBigInt32(x)
#define htole32(x) OSSwapHostToLittleInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define le32toh(x) OSSwapLittleToHostInt32(x)

#define htobe64(x) OSSwapHostToBigInt64(x)
#define htole64(x) OSSwapHostToLittleInt64(x)
#define be64toh(x) OSSwapBigToHostInt64(x)
#define le64toh(x) OSSwapLittleToHostInt64(x)
#endif
#endif

struct mmsghdr { struct msghdr msg_hdr; unsigned int msg_len; };
static inline int recvmmsg(int fd, struct mmsghdr *messages, unsigned int count, int flags, struct timespec *) {
  unsigned int received = 0;
  for (; received < count; ++received) {
    ssize_t length = recvmsg(fd, &messages[received].msg_hdr, flags | (received ? MSG_DONTWAIT : 0));
    if (length < 0) return received ? (int)received : -1;
    messages[received].msg_len = (unsigned int)length;
  }
  return (int)received;
}
