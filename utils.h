#ifndef UTILS_H
#define UTILS_H

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <cstdarg>
#include <cstdio> 
#include <cstring>
#include <cerrno>
#include <stdexcept>
#include <string>
#include <unistd.h>

// Helper function to handle errors and print a formatted message
void die(const char* fmt, ...);

// Wraps malloc with error checking: throws an exception if malloc fails
void* xmalloc(size_t size);

// Dies (throws an exception) after printing the current perror status
// prefixed with msg
void perror_die(const std::string& msg);

// Reports a peer connection to stdout. sa is the data populated by a successful
// accept() call
void report_peer_connected(const sockaddr_in& sa, socklen_t salen);

// Creates a bound and listening INET socket on the given port number. Returns
// the socket fd when successful; throws an exception in case of errors
int listen_inet_socket(int portnum);

// Sets the given socket into non-blocking mode
void make_socket_non_blocking(int sockfd);

#endif /* UTILS_H */