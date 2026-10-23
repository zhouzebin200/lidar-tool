#pragma once
#include <sys/socket.h>
#include <time.h>
#include <errno.h>
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
