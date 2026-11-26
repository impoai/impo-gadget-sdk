#pragma once
#include <sys/socket.h>
int sal_setsockopt(int fd, int level, int option, const void *value, socklen_t size);
