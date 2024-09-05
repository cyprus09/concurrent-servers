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

void serve_connection(int sockfd) {
    // Clients attempting to connect and send data will succeed even before the
    // connection is accept()-ed by the server. Therefore, to better simulate
    // blocking of other clients while one is being served, do this "ack" from the
    // server which the client expects to see before proceeding.
    if(send(sockfd, "*", 1, 0) < 1)
      perror_die("send");
    
    ProcessingState state = ProcessingState::WAIT_FOR_MSG;

    while(true) {
      uint8_t buf[1024];
      int len = recv(sockfd, buf, sizeof(buf), 0);
      if(len < 0) 
        perror_die("recv");
      else if(len == 0)
        break;
      
      for(int i = 0; i < len; i++) {
        switch (state)
        {
        case ProcessingState::WAIT_FOR_MSG:
          if(buf[i] == '^')
            state = ProcessingState::IN_MSG;
          break;
        case ProcessingState::IN_MSG:
          if(buf[i] == '$') 
            state = ProcessingState::WAIT_FOR_MSG;
          else {
            buf[i] += 1;
            if(send(sockfd, &buf[i], 1, 0) < 1) {
              perror("send error");
              close(sockfd);
              return;
            }
          }
          break;
        }
      }
    }
  close(sockfd);
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  int portnum = 9090;
  if(argc >= 2)
    portnum = std::atoi(argv[1]);

  std::cout << "Serving on port " << portnum << std::endl;

  int sockfd = listen_inet_socket(portnum);

  while(true) {
    struct sockaddr_in peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);

    int newsockfd = accept(sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);

    if(newsockfd < 0)
      perror_die("ERROR on accept");

    report_peer_connected(&peer_addr, peer_addr_len);
    serve_connection(newsockfd);
    std::cout << "peer done" << std::endl;
  }

  return 0;
}