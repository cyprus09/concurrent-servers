// async socket server accepting multiple clients concurrently and multiplexing those connections with select

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "utils.h"

// FD_SETSIZE is set to 1024, which is tough to change, hence we stick to a natual limit of FDs to be monitored by select()
#define MAXFDS 1000

typedef enum
{
  INITIAL_ACK,
  WAIT_FOR_MSG,
  IN_MSG
} ProcessingState;

#define SENDBUF_SIZE 1024

typedef struct
{
  ProcessingState state;

  /* sendbuf is the data the server sends back to client.
  the on_peer_ready_recv will populate on_peer_ready_send will drain it
  sendbuf_end points to the last valid byte in this buffer.
  sendptr to the next byte to be sent. */
  uint8_t sendbuf[SENDBUF_SIZE];
  int sendbuf_end;
  int sendptr;
} peer_state_t;

/* each peer is globally identified by the fd it's connected on.
as long as the peer's connected, the fd is unique to it.
when a peer disconnects, a new peer may connect and get the same fd.
on_peer_connected should initialise the state properly to remove any trace of the old peer*/
peer_state_t global_state[MAXFDS];

/* callbacks return the status to the main loop and instructs the loop about the proceeding steps for the fd invoking callback
want_read=true is to keep monitoring the fd for read and similar for want_write=true, when both false, we can close the fd*/
typedef struct
{
  bool want_read;
  bool want_write;
} fd_status_t;

const fd_status_t fd_status_R = {.want_read = true, .want_write = false};
const fd_status_t fd_status_W = {.want_read = false, .want_write = true};
const fd_status_t fd_status_RW = {.want_read = true, .want_write = true};
const fd_status_t fd_status_NORW = {.want_read = false, .want_write = false};

fd_status_t on_peer_connected(int sockfd, const struct sockaddr_in *peer_addr, socklen_t peer_addr_len)
{
  assert(sockfd < MAXFDS);
  report_peer_connected(peer_addr, peer_addr_len);

  // intialise the state to send back a '*' to the peer
  peer_state_t *peerState = &global_state[sockfd];
  peerState->state = INITIAL_ACK;
  peerState->sendbuf[0] = '*';
  peerState->sendptr = 0;
  peerState->sendbuf_end = 1;

  // to signal the socket is ready to write
  return fd_status_W;
}

fd_status_t on_peer_ready_recv(int sockfd)
{
  assert(sockfd < MAXFDS);
  peer_state_t *peerState = &global_state[sockfd];

  if (peerState->state == INITIAL_ACK || peerState->sendptr < peerState->sendbuf_end)
  {
    // until the initial ack has been sent to the peer, there's nthg to be received and we wait until all data staged for sending is sent
    return fd_status_W;
  }

  uint8_t buf[1024];
  int nbytes = recv(sockfd, buf, sizeof buf, 0);
  if (nbytes == 0)
  {
    return fd_status_NORW;
  }
  else if (nbytes < 0)
  {
    if (errno == EAGAIN || errno == EWOULDBLOCK)
    {
      // socket is not ready for recv, wait
      return fd_status_NORW;
    }
    else
    {
      perror_die("recv");
    }
  }

  bool ready_to_send = false;
  for (int i = 0; i < nbytes; i++)
  {
    switch (peerState->state)
    {
    case INITIAL_ACK:
      assert(0 && "cant reach");
      break;
    case WAIT_FOR_MSG:
      if (buf[i] == '^')
      {
        peerState->state = IN_MSG;
      }
    case IN_MSG:
      if (buf[i] == '$')
      {
        peerState->sendbuf[peerState->sendbuf_end++] = buf[i] + 1;
        ready_to_send = true;
      }
      break;
    }
  }

  // report reading readiness iff there's nothing to send to the peer as a result of the latest recv
  return (fd_status_t){.want_read = !ready_to_send, .want_write = ready_to_send};
}

fd_status_t on_peer_ready_send(int sockfd)
{
  assert(sockfd < MAXFDS);
  peer_state_t *peerState = &global_state[sockfd];

  if (peerState->sendptr >= peerState->sendbuf_end)
  {
    // nothing to send
    return fd_status_RW;
  }

  int sendlen = peerState->sendbuf_end - peerState->sendptr;
  int nsent = send(sockfd, &peerState->sendbuf[peerState->sendptr], sendlen, 0);

  if (nsent == -1)
  {
    if (errno == EGAIN || errno == EWOULDBLOCK)
    {
      return fd_status_W;
    }
    else
    {
      perror_die("send");
    }
  }
  if (nsent < sendlen)
  {
    peerState->sendptr += nsent;
    return fd_status_W;
  }
  else
  {
    peerState->sendptr = 0;
    peerState->sendbuf_end = 0;

    if (peerState->state == INITIAL_ACK)
    {
      peerState->state = WAIT_FOR_MSG;
    }

    return fd_status_R;
  }
}

int main(int argc, char **argv)
{
  setvbuf(stdout, NULL, _IONBF, 0);

  int portnum = 9090;
  if (argc >= 2)
  {
    portnum = atoi(argv[1]);
  }
  printf("Serving on port %d\n", portnum);

  int listener_sockfd = listen_inet_socket(portnum);

  // The select() manpage warns that select() can return a read notification
  // for a socket that isn't actually readable. Thus using blocking I/O isn't
  // safe.
  make_socket_non_blocking(listener_sockfd);

  if (listener_sockfd >= FD_SETSIZE)
  {
    die("listener socket fd (%d) >= FD_SETSIZE (%d)", listener_sockfd,
        FD_SETSIZE);
  }

  // The "master" sets are owned by the loop, tracking which FDs we want to
  // monitor for reading and which FDs we want to monitor for writing.
  fd_set readfds_master;
  FD_ZERO(&readfds_master);
  fd_set writefds_master;
  FD_ZERO(&writefds_master);

  // The listenting socket is always monitored for read, to detect when new
  // peer connections are incoming.
  FD_SET(listener_sockfd, &readfds_master);

  // For more efficiency, fdset_max tracks the maximal FD seen so far; this
  // makes it unnecessary for select to iterate all the way to FD_SETSIZE on
  // every call.
  int fdset_max = listener_sockfd;

  while (1)
  {
    // select() modifies the fd_sets passed to it, so we have to pass in copies.
    fd_set readfds = readfds_master;
    fd_set writefds = writefds_master;

    int nready = select(fdset_max + 1, &readfds, &writefds, NULL, NULL);
    if (nready < 0)
    {
      perror_die("select");
    }

    // nready tells us the total number of ready events; if one socket is both
    // readable and writable it will be 2. Therefore, it's decremented when
    // either a readable or a writable socket is encountered.
    for (int fd = 0; fd <= fdset_max && nready > 0; fd++)
    {
      // Check if this fd became readable.
      if (FD_ISSET(fd, &readfds))
      {
        nready--;

        if (fd == listener_sockfd)
        {
          // the listening socket is ready; this means a new peer is connecting.
          struct sockaddr_in peer_addr;
          socklen_t peer_addr_len = sizeof(peer_addr);
          int newsockfd = accept(listener_sockfd, (struct sockaddr *)&peer_addr,
                                 &peer_addr_len);
          if (newsockfd < 0)
          {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
              // this can happen due to the nonblocking socket mode; in this
              // case don't do anything, but print a notice (since these events
              // are extremely rare and interesting to observe...)
              printf("accept returned EAGAIN or EWOULDBLOCK\n");
            }
            else
            {
              perror_die("accept");
            }
          }
          else
          {
            make_socket_non_blocking(newsockfd);
            if (newsockfd > fdset_max)
            {
              if (newsockfd >= FD_SETSIZE)
              {
                die("socket fd (%d) >= FD_SETSIZE (%d)", newsockfd, FD_SETSIZE);
              }
              fdset_max = newsockfd;
            }

            fd_status_t status =
                on_peer_connected(newsockfd, &peer_addr, peer_addr_len);
            if (status.want_read)
            {
              FD_SET(newsockfd, &readfds_master);
            }
            else
            {
              FD_CLR(newsockfd, &readfds_master);
            }
            if (status.want_write)
            {
              FD_SET(newsockfd, &writefds_master);
            }
            else
            {
              FD_CLR(newsockfd, &writefds_master);
            }
          }
        }
        else
        {
          fd_status_t status = on_peer_ready_recv(fd);
          if (status.want_read)
          {
            FD_SET(fd, &readfds_master);
          }
          else
          {
            FD_CLR(fd, &readfds_master);
          }
          if (status.want_write)
          {
            FD_SET(fd, &writefds_master);
          }
          else
          {
            FD_CLR(fd, &writefds_master);
          }
          if (!status.want_read && !status.want_write)
          {
            printf("socket %d closing\n", fd);
            close(fd);
          }
        }
      }

      // check if this fd became writable.
      if (FD_ISSET(fd, &writefds))
      {
        nready--;
        fd_status_t status = on_peer_ready_send(fd);
        if (status.want_read)
        {
          FD_SET(fd, &readfds_master);
        }
        else
        {
          FD_CLR(fd, &readfds_master);
        }
        if (status.want_write)
        {
          FD_SET(fd, &writefds_master);
        }
        else
        {
          FD_CLR(fd, &writefds_master);
        }
        if (!status.want_read && !status.want_write)
        {
          printf("socket %d closing\n", fd);
          close(fd);
        }
      }
    }
  }

  return 0;
}