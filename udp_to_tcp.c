#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <stdarg.h>

#define MAX_UDP_PACKET_SIZE 120
#define TCP_RETRY_DELAY 5
#define POLL_TIMEOUT 1000

int udp_sock = -1;
int tcp_sock = -1;
FILE* log_file = NULL;
char prefix[5];

enum {
    FDS_UDP_SOCK,
    FDS_TCP_SOCK,
    MAX_POLL_FDS
};

void log_message(const char* format, ...);
void log_data(const char* direction, const unsigned char* data, int length);
int setup_udp_socket(const char* ip_port);
int setup_tcp_socket(const char* ip_port);
int make_socket_non_blocking(int sock);
void close_tcp_connection();

int main(int argc, char* argv[]) {
    if (argc != 5) {
        fprintf(stderr, "Usage: %s <UDP_IP:PORT> <TCP_IP:PORT> <LOG_FILE> <PREFIX>\n", argv[0]);
        return 1;
    }

    const char* udp_ip_port = argv[1];
    const char* tcp_ip_port = argv[2];
    const char* log_filename = argv[3];
    const char* prefix_arg = argv[4];

    if (strlen(prefix_arg) != 4) {
        fprintf(stderr, "ERROR: Prefix must be exactly 4 symbols\n");
        return 1;
    }
    strncpy(prefix, prefix_arg, 4);
    prefix[4] = '\0';

    log_file = fopen(log_filename, "a");
    if (!log_file) {
        fprintf(stderr, "ERROR: Cannot open log file %s: %s\n", log_filename, strerror(errno));
        return 1;
    }

    udp_sock = setup_udp_socket(udp_ip_port);
    if (udp_sock < 0) {
        log_message("ERROR: Failed to set up UDP socket");
        fclose(log_file);
        return 1;
    }

    struct pollfd fds[MAX_POLL_FDS];
    int tcp_connection_active = 0;
    time_t last_tcp_attempt = 0;

    log_message("INFO: Starting UDP to TCP relay with prefix \"%s\"", prefix);

    while (1) {
        int nfds = 0;

        fds[FDS_UDP_SOCK].fd = udp_sock;
        fds[FDS_UDP_SOCK].events = POLLIN;
        fds[FDS_UDP_SOCK].revents = 0;
        nfds++;

        if (tcp_sock >= 0) {
            fds[FDS_TCP_SOCK].fd = tcp_sock;
            fds[FDS_TCP_SOCK].events = POLLIN;
            fds[FDS_TCP_SOCK].revents = 0;
            nfds++;
        }

        int poll_result = poll(fds, nfds, POLL_TIMEOUT);

        if (poll_result < 0 && errno != EINTR) {
            log_message("ERROR: poll() failed: %s", strerror(errno));
            continue;
        }

        time_t current_time = time(NULL);
        if (tcp_sock < 0 && (current_time - last_tcp_attempt >= TCP_RETRY_DELAY)) {
            tcp_sock = setup_tcp_socket(tcp_ip_port);
            last_tcp_attempt = current_time;

            if (tcp_sock >= 0) {
                tcp_connection_active = 1;
                log_message("INFO: TCP connection established");
            }
        }

        if (fds[FDS_UDP_SOCK].revents & POLLIN) {
            struct sockaddr_storage sender_addr;
            socklen_t sender_addr_len = sizeof(sender_addr);
            unsigned char buffer[MAX_UDP_PACKET_SIZE];

            int bytes_received = recvfrom(udp_sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&sender_addr, &sender_addr_len);

            if (bytes_received > 0) {
                log_data("UDP RX", buffer, bytes_received);

                if (tcp_connection_active) {
                    unsigned char buffer_with_prefix[4 + MAX_UDP_PACKET_SIZE];
                    memcpy(buffer_with_prefix, prefix, 4);
                    memcpy(buffer_with_prefix + 4, buffer, bytes_received);

                    int total_length = bytes_received + 4;
                    int bytes_sent = send(tcp_sock, buffer_with_prefix, total_length, 0);

                    if (bytes_sent < 0) {
                        log_message("ERROR: Failed to send data to TCP: %s", strerror(errno));
                        close_tcp_connection();
                        tcp_connection_active = 0;
                    } else if (bytes_sent != total_length) {
                        log_message("WARNING: Partial TCP send: %d of %d bytes", bytes_sent, total_length);
                    } else {
                        log_data("TCP TX", buffer_with_prefix, total_length);
                    }
                } else {
                    log_message("INFO: Discarded UDP packet, TCP connection not active");
                }
            } else if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                log_message("ERROR: UDP recvfrom() failed: %s", strerror(errno));
            }
        }

        if (nfds > 1 && (fds[FDS_TCP_SOCK].revents & POLLIN)) {
            unsigned char buffer[1024];
            int bytes_received = recv(tcp_sock, buffer, sizeof(buffer), 0);

            if (bytes_received > 0) {
                log_data("TCP RX", buffer, bytes_received);
                log_message("INFO: Discarded %d bytes from TCP connection", bytes_received);
            } else if (bytes_received == 0) {
                log_message("INFO: TCP connection closed by server");
                close_tcp_connection();
                tcp_connection_active = 0;
            } else if (bytes_received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                log_message("ERROR: TCP recv() failed: %s", strerror(errno));
                close_tcp_connection();
                tcp_connection_active = 0;
            }
        }

        if (nfds > 1 && (fds[FDS_TCP_SOCK].revents & (POLLERR | POLLHUP | POLLNVAL))) {
            log_message("INFO: TCP connection error detected");
            close_tcp_connection();
            tcp_connection_active = 0;
        }
    }

    close(udp_sock);
    if (tcp_sock >= 0) {
        close(tcp_sock);
    }
    fclose(log_file);

    return 0;
}

int setup_udp_socket(const char* ip_port) {
    char ip_copy[128];
    strncpy(ip_copy, ip_port, sizeof(ip_copy) - 1);
    ip_copy[sizeof(ip_copy) - 1] = '\0';

    char* colon = strchr(ip_copy, ':');
    if (!colon) {
        log_message("ERROR: Invalid UDP address format. Use IP:PORT");
        return -1;
    }

    *colon = '\0';
    char* ip = ip_copy;
    int port = atoi(colon + 1);

    if (port <= 0 || port > 65535) {
        log_message("ERROR: Invalid UDP port number");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        log_message("ERROR: Cannot create UDP socket: %s", strerror(errno));
        return -1;
    }

    if (make_socket_non_blocking(sock) < 0) {
        close(sock);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        log_message("ERROR: Invalid UDP IP address: %s", ip);
        close(sock);
        return -1;
    }

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        log_message("ERROR: Cannot bind UDP socket to %s:%d: %s", ip, port, strerror(errno));
        close(sock);
        return -1;
    }

    log_message("INFO: UDP socket bound to %s:%d", ip, port);
    return sock;
}

int setup_tcp_socket(const char* ip_port) {
    char ip_copy[128];
    strncpy(ip_copy, ip_port, sizeof(ip_copy) - 1);
    ip_copy[sizeof(ip_copy) - 1] = '\0';

    char* colon = strchr(ip_copy, ':');
    if (!colon) {
        log_message("ERROR: Invalid TCP address format. Use IP:PORT");
        return -1;
    }

    *colon = '\0';
    char* ip = ip_copy;
    int port = atoi(colon + 1);

    if (port <= 0 || port > 65535) {
        log_message("ERROR: Invalid TCP port number");
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        log_message("ERROR: Cannot create TCP socket: %s", strerror(errno));
        return -1;
    }

    if (make_socket_non_blocking(sock) < 0) {
        close(sock);
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &server_addr.sin_addr) <= 0) {
        log_message("ERROR: Invalid TCP IP address: %s", ip);
        close(sock);
        return -1;
    }

    log_message("INFO: Attempting TCP connection to %s:%d", ip, port);
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        if (errno == EINPROGRESS) {
            log_message("INFO: TCP connection in progress");
            return sock;
        } else {
            log_message("ERROR: TCP connect() failed: %s", strerror(errno));
            close(sock);
            return -1;
        }
    }

    log_message("INFO: TCP connection established immediately");
    return sock;
}

int make_socket_non_blocking(int sock) {
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        log_message("ERROR: fcntl(F_GETFL) failed: %s", strerror(errno));
        return -1;
    }

    flags |= O_NONBLOCK;
    if (fcntl(sock, F_SETFL, flags) < 0) {
        log_message("ERROR: fcntl(F_SETFL) failed: %s", strerror(errno));
        return -1;
    }

    return 0;
}

void close_tcp_connection() {
    if (tcp_sock >= 0) {
        close(tcp_sock);
        tcp_sock = -1;
        log_message("INFO: TCP connection closed");
    }
}

void log_message(const char* format, ...) {
    if (!log_file) return;

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file);
}

void log_data(const char* direction, const unsigned char* data, int length) {
    if (!log_file) return;

    fprintf(log_file, "%s (%d): ", direction, length);

    for (int i = 0; i < length; i++) {
        fprintf(log_file, "%02X ", data[i]);
    }

    fprintf(log_file, "\n");
    fflush(log_file);
}