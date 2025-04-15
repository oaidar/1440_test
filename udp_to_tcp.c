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
#include <pthread.h>

#define MAX_UDP_PACKET_SIZE 120
#define TCP_RETRY_DELAY 5
#define QUEUE_SIZE 100

typedef struct {
    unsigned char data[MAX_UDP_PACKET_SIZE];
    int length;
} packet_t;

typedef struct {
    packet_t packets[QUEUE_SIZE];
    int head;
    int tail;
    int count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} packet_queue_t;

int udp_sock = -1;
int tcp_sock = -1;
int shutdown_flag = 0;
int tcp_connection_active = 0;
FILE* log_file = NULL;
char prefix[5];
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
packet_queue_t queue;

void log_message(const char* format, ...);
void log_data(const char* direction, const unsigned char* data, int length);
int setup_udp_socket(const char* ip_port);
int setup_tcp_socket(const char* ip_port);
void close_tcp_connection();
void initialize_queue(packet_queue_t* q);
int enqueue_packet(packet_queue_t* q, const unsigned char* data, int length);
int dequeue_packet(packet_queue_t* q, unsigned char* data, int* length);
void* udp_receiver_thread(void* arg);
void* tcp_sender_thread(void* arg);

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

    initialize_queue(&queue);

    log_message("INFO: Starting UDP to TCP relay with prefix \"%s\"", prefix);

    pthread_t udp_thread, tcp_thread;
    pthread_create(&udp_thread, NULL, udp_receiver_thread, (void*)udp_ip_port);
    pthread_create(&tcp_thread, NULL, tcp_sender_thread, (void*)tcp_ip_port);

    pthread_join(udp_thread, NULL);
    pthread_join(tcp_thread, NULL);

    if (udp_sock >= 0) close(udp_sock);
    if (tcp_sock >= 0) close(tcp_sock);

    pthread_mutex_destroy(&queue.mutex);
    pthread_cond_destroy(&queue.not_empty);
    pthread_cond_destroy(&queue.not_full);
    pthread_mutex_destroy(&log_mutex);

    if (log_file) fclose(log_file);

    return 0;
}

void initialize_queue(packet_queue_t* q) {
    q->head = 0;
    q->tail = 0;
    q->count = 0;
    pthread_mutex_init(&q->mutex, NULL);
    pthread_cond_init(&q->not_empty, NULL);
    pthread_cond_init(&q->not_full, NULL);
}

int enqueue_packet(packet_queue_t* q, const unsigned char* data, int length) {
    pthread_mutex_lock(&q->mutex);

    while (q->count >= QUEUE_SIZE && !shutdown_flag) {
        pthread_cond_wait(&q->not_full, &q->mutex);
    }

    if (shutdown_flag) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    memcpy(q->packets[q->tail].data, data, length);
    q->packets[q->tail].length = length;
    q->tail = (q->tail + 1) % QUEUE_SIZE;
    q->count++;

    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

int dequeue_packet(packet_queue_t* q, unsigned char* data, int* length) {
    pthread_mutex_lock(&q->mutex);

    while (q->count <= 0 && !shutdown_flag) {
        pthread_cond_wait(&q->not_empty, &q->mutex);
    }

    if (shutdown_flag && q->count <= 0) {
        pthread_mutex_unlock(&q->mutex);
        return -1;
    }

    memcpy(data, q->packets[q->head].data, q->packets[q->head].length);
    *length = q->packets[q->head].length;
    q->head = (q->head + 1) % QUEUE_SIZE;
    q->count--;

    pthread_cond_signal(&q->not_full);
    pthread_mutex_unlock(&q->mutex);
    return 0;
}

void* udp_receiver_thread(void* arg) {
    const char* udp_ip_port = (const char*)arg;

    udp_sock = setup_udp_socket(udp_ip_port);
    if (udp_sock < 0) {
        log_message("ERROR: Failed to set up UDP socket");
        shutdown_flag = 1;
        pthread_cond_signal(&queue.not_empty);
        return NULL;
    }

    struct sockaddr_storage sender_addr;
    socklen_t sender_addr_len;
    unsigned char buffer[MAX_UDP_PACKET_SIZE];
    int bytes_received;

    while (!shutdown_flag) {
        sender_addr_len = sizeof(sender_addr);
        bytes_received = recvfrom(udp_sock, buffer, sizeof(buffer), 0, 
                                  (struct sockaddr*)&sender_addr, &sender_addr_len);

        if (bytes_received > 0) {
            log_data("UDP RX", buffer, bytes_received);

            pthread_mutex_lock(&queue.mutex);
            int should_enqueue = tcp_connection_active;
            pthread_mutex_unlock(&queue.mutex);

            if (should_enqueue) {
                if (enqueue_packet(&queue, buffer, bytes_received) < 0) {
                    break;
                }
            } else {
                log_message("INFO: Discarded UDP packet, TCP connection not active");
            }
        } else if (bytes_received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                log_message("ERROR: UDP recvfrom() failed: %s", strerror(errno));
                if (errno != EINTR) {
                    sleep(1);
                }
            }
        }
    }

    close(udp_sock);
    udp_sock = -1;
    return NULL;
}

void* tcp_sender_thread(void* arg) {
    const char* tcp_ip_port = (const char*)arg;
    time_t last_tcp_attempt = 0;
    unsigned char buffer[MAX_UDP_PACKET_SIZE];
    unsigned char buffer_with_prefix[4 + MAX_UDP_PACKET_SIZE];
    int bytes_read;
    struct pollfd pfd;

    memcpy(buffer_with_prefix, prefix, 4);

    while (!shutdown_flag) {
        time_t current_time = time(NULL);

        if (tcp_sock < 0 && (current_time - last_tcp_attempt >= TCP_RETRY_DELAY)) {
            tcp_sock = setup_tcp_socket(tcp_ip_port);
            last_tcp_attempt = current_time;

            pthread_mutex_lock(&queue.mutex);
            tcp_connection_active = (tcp_sock >= 0);
            pthread_mutex_unlock(&queue.mutex);

            if (tcp_connection_active) {
                log_message("INFO: TCP connection established");
            }
        }

        if (tcp_connection_active) {
            pfd.fd = tcp_sock;
            pfd.events = POLLIN;

            if (poll(&pfd, 1, 0) > 0) {
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    log_message("INFO: TCP connection error detected");
                    close_tcp_connection();
                    pthread_mutex_lock(&queue.mutex);
                    tcp_connection_active = 0;
                    pthread_mutex_unlock(&queue.mutex);
                    continue;
                }

                if (pfd.revents & POLLIN) {
                    unsigned char tcp_buffer[1024];
                    int tcp_bytes_received = recv(tcp_sock, tcp_buffer, sizeof(tcp_buffer), 0);

                    if (tcp_bytes_received > 0) {
                        log_data("TCP RX", tcp_buffer, tcp_bytes_received);
                        log_message("INFO: Discarded %d bytes from TCP connection", tcp_bytes_received);
                    } else if (tcp_bytes_received == 0) {
                        log_message("INFO: TCP connection closed by server");
                        close_tcp_connection();
                        pthread_mutex_lock(&queue.mutex);
                        tcp_connection_active = 0;
                        pthread_mutex_unlock(&queue.mutex);
                        continue;
                    } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
                        log_message("ERROR: TCP recv() failed: %s", strerror(errno));
                        close_tcp_connection();
                        pthread_mutex_lock(&queue.mutex);
                        tcp_connection_active = 0;
                        pthread_mutex_unlock(&queue.mutex);
                        continue;
                    }
                }
            }

            pthread_mutex_lock(&queue.mutex);
            int has_packets = (queue.count > 0);
            pthread_mutex_unlock(&queue.mutex);

            if (has_packets) {
                if (dequeue_packet(&queue, buffer, &bytes_read) == 0) {
                    memcpy(buffer_with_prefix + 4, buffer, bytes_read);
                    int total_length = bytes_read + 4;

                    int bytes_sent = send(tcp_sock, buffer_with_prefix, total_length, 0);

                    if (bytes_sent < 0) {
                        if (errno != EAGAIN && errno != EWOULDBLOCK) {
                            log_message("ERROR: Failed to send data to TCP: %s", strerror(errno));
                            close_tcp_connection();
                            pthread_mutex_lock(&queue.mutex);
                            tcp_connection_active = 0;
                            pthread_mutex_unlock(&queue.mutex);
                        }
                    } else if (bytes_sent != total_length) {
                        log_message("WARNING: Partial TCP send: %d of %d bytes", bytes_sent, total_length);
                    } else {
                        log_data("TCP TX", buffer_with_prefix, total_length);
                    }
                }
            } else {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                ts.tv_sec += 1;

                pthread_mutex_lock(&queue.mutex);
                if (queue.count == 0 && !shutdown_flag) {
                    pthread_cond_timedwait(&queue.not_empty, &queue.mutex, &ts);
                }
                pthread_mutex_unlock(&queue.mutex);
            }
        } else {
            pfd.fd = -1;
            pfd.events = 0;
            poll(&pfd, 1, 100);
        }
    }

    close_tcp_connection();
    return NULL;
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

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) {
        log_message("ERROR: fcntl(F_GETFL) failed: %s", strerror(errno));
        close(sock);
        return -1;
    }

    flags |= O_NONBLOCK;
    if (fcntl(sock, F_SETFL, flags) < 0) {
        log_message("ERROR: fcntl(F_SETFL) failed: %s", strerror(errno));
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

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        log_message("ERROR: fcntl() failed: %s", strerror(errno));
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
        if (errno != EINPROGRESS) {
            log_message("ERROR: TCP connect() failed: %s", strerror(errno));
            close(sock);
            return -1;
        }
        log_message("INFO: TCP connection in progress");
    } else {
        log_message("INFO: TCP connection established immediately");
    }

    return sock;
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

    pthread_mutex_lock(&log_mutex);

    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

    fprintf(log_file, "[%s] ", timestamp);

    va_list args;
    va_start(args, format);
    vfprintf(log_file, format, args);
    va_end(args);

    fprintf(log_file, "\n");
    fflush(log_file);

    pthread_mutex_unlock(&log_mutex);
}

void log_data(const char* direction, const unsigned char* data, int length) {
    if (!log_file) return;

    pthread_mutex_lock(&log_mutex);

    time_t now = time(NULL);
    struct tm* timeinfo = localtime(&now);
    char timestamp[20];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);

    fprintf(log_file, "[%s] %s (%d): ", timestamp, direction, length);

    for (int i = 0; i < length; i++) {
        fprintf(log_file, "%02X ", data[i]);
    }

    fprintf(log_file, "\n");
    fflush(log_file);

    pthread_mutex_unlock(&log_mutex);
}
