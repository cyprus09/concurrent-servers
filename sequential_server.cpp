// this server sets the problem statement for the project since it handles one client at a time leading to high latency between server-client communications since it follows the state machine
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <iostream>
#include <arpa/inet.h>

#include "utils.h"

enum class ProcessingState
{
  WAIT_FOR_MSG,
  IN_MSG
};

void serve_connection(int sockfd)
