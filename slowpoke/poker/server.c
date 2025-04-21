#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <ctype.h>
#include <netdb.h>
#include <sys/select.h>
#include <netinet/tcp.h> 

#define POKER_PORT 5550
#define POKER_PORT_STR "5550"
#define FIFO_PATH "/tmp/slowpoke_fifo"
#define FIFO_RECOVER_PATH "/tmp/slowpoke_fifo_recover"
#define DEBUG 0

int* neighbor_conns = NULL;
// pthread_mutex_t neighbor_lock = PTHREAD_MUTEX_INITIALIZER;
ssize_t num_neighbors = 0;
typedef struct {
    int64_t phase;
    int64_t delay_nanos;
} poker_message;
int poker_phase = 0;
pthread_mutex_t poker_phase_lock = PTHREAD_MUTEX_INITIALIZER;
int64_t accumulated_nano_sleep = 0;
// pthread_mutex_t accumulated_nano_sleep_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct{
    pid_t pgid;
    int sockfd;
} client_info;
// Global variables for thread communication
int child_exited = 0;
int child_exit_status = 0;
long long sleep_surplus = 0;

// Helper functions for 64-bit network/host byte order conversion
#if __BYTE_ORDER == __LITTLE_ENDIAN
uint64_t htonll(uint64_t value) {
    return (((uint64_t)htonl((uint32_t)(value & 0xFFFFFFFF))) << 32) |
           htonl((uint32_t)(value >> 32));
}

uint64_t ntohll(uint64_t value) {
    return (((uint64_t)ntohl((uint32_t)(value & 0xFFFFFFFF))) << 32) |
           ntohl((uint32_t)(value >> 32));
}
#else
uint64_t htonll(uint64_t value) {
    return value;
}

uint64_t ntohll(uint64_t value) {
    return value;
}
#endif

ssize_t send_poker_message(int sockfd, const poker_message *msg) {
    poker_message net_msg;
    net_msg.phase = htonll(msg->phase);
    net_msg.delay_nanos = htonll(msg->delay_nanos);
    return send(sockfd, &net_msg, sizeof(net_msg), 0);
}

ssize_t recv_poker_message(int sockfd, poker_message *msg) {
    poker_message net_msg;
    fflush(stdout);
    ssize_t n = recv(sockfd, &net_msg, sizeof(net_msg), MSG_WAITALL);
    if (n == sizeof(net_msg)) {
        msg->phase = ntohll(net_msg.phase);
        msg->delay_nanos = ntohll(net_msg.delay_nanos);
    }
    return n;
}

int propagate_poker_message_to_neighbors(poker_message *msg) {
    // pthread_mutex_lock(&neighbor_lock);
    if (DEBUG) {
        printf("Propagating message to neighbors: phase=%lld, delay_nanos=%lld, num_neighbors=%d\n", msg->phase, msg->delay_nanos, num_neighbors);
        fflush(stdout);
    }
    for (int i = 0; i < num_neighbors; i++) {
        // Convert to network byte order
        ssize_t sent = send_poker_message(neighbor_conns[i], msg);
        if (sent != sizeof(poker_message)) {
            if (sent == -1) {
                fprintf(stdout, "Failed to send message to neighbor %d: %s\n", i, strerror(errno));
                fflush(stdout);
            } else {
                fprintf(stdout, "Partial send to neighbor %d: %zd bytes\n", i, sent);
                fflush(stdout);
            }
        }
    }
    // pthread_mutex_unlock(&neighbor_lock);
    return 0;
}

static long long get_current_time_ns() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void printtime(void) {
    printf("time lld\n", get_current_time_ns());
}

void precise_sleep(long long sleep_ns) {
    long long start_time = get_current_time_ns();
    long long elapsed_time = 0;

    /* long long common = sleep_ns < sleep_surplus ? sleep_ns : sleep_surplus; */
    /* sleep_surplus -= common; */
    /* sleep_ns -= common; */
    while (elapsed_time < sleep_ns) {
        long long remaining_time = sleep_ns - elapsed_time;

        // Use nanosleep for high-resolution sleep
        struct timespec req = {
            .tv_sec = remaining_time / 1000000000LL,
            .tv_nsec = remaining_time % 1000000000LL
        };

        // Sleep for the remaining time
        if (nanosleep(&req, NULL) == -1) {
            if (errno == EINTR) {
                // If interrupted, recalculate elapsed time and continue
                elapsed_time = get_current_time_ns() - start_time;
                continue;
            } else {
                perror("nanosleep");
                exit(EXIT_FAILURE);
            }
        }

        // Calculate elapsed time after sleep
        elapsed_time = get_current_time_ns() - start_time;
    }
    /* sleep_surplus += elapsed_time - sleep_ns; */
    /* printf("done sleeping, surplus %lld\n", sleep_surplus); */
}

void* do_delay(int64_t nanosleep, pid_t child_pgid) {
    pthread_mutex_lock(&poker_phase_lock);
    if (DEBUG) {
        printf("do_delay: nanosleep=%lld, child_pgid=%d\n", nanosleep, child_pgid);
        fflush(stdout);
    }
    accumulated_nano_sleep += nanosleep;
    fflush(stdout);
    int64_t start_time = get_current_time_ns();
    // printf("[before] accumulated_nano_sleep: %lld, start_time: %lld\n", accumulated_nano_sleep, start_time);
    if (kill(-child_pgid, SIGSTOP) == -1) {
        printf("error in stopping");
        fflush(stdout);
    }
    // precise_sleep(nanosleep);
    precise_sleep(accumulated_nano_sleep);
    int64_t end_time = get_current_time_ns();
    accumulated_nano_sleep -= (end_time - start_time);
    if (kill(-child_pgid, SIGCONT) == -1) {
        printf("error in conting");
        fflush(stdout);
    }
    // printf("[after] accumulated_nano_sleep: %lld, end_time: %lld\n", accumulated_nano_sleep, end_time);
    pthread_mutex_unlock(&poker_phase_lock);
}

void* handle_client(void* arg) {
    client_info* client = (client_info*)arg;
    int client_sock = client->sockfd;
    pid_t child_pgid = client->pgid;
    free(arg);

    poker_message msg;

    while (recv_poker_message(client_sock, &msg) != 0) {
        int phase_seen = 1;
        pthread_mutex_lock(&poker_phase_lock);
        if (poker_phase < msg.phase) {
            poker_phase = msg.phase;
            phase_seen = 0;
        }
        pthread_mutex_unlock(&poker_phase_lock);
        if (DEBUG) {
            printf("Received message: phase=%lld, delay_nanos=%lld, seen=%d\n", msg.phase, msg.delay_nanos, phase_seen);
            fflush(stdout);
        }
        if (!phase_seen) {
            // Do the delay
            propagate_poker_message_to_neighbors(&msg);
            do_delay(msg.delay_nanos, child_pgid);
        }

    }

    printf("Client disconnected\n");
    close(client_sock);
    return NULL;
}

void *poker_server(void *arg) {
    pid_t child_pgid = *((pid_t *)arg);
    int server_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (server_sock == -1) {
        fprintf(stderr, "[poker] Failed to create socket for listener\n");
        perror("socket");
        exit(1);
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(POKER_PORT),
        .sin_addr.s_addr = INADDR_ANY
    };

    printf("[poker] Binding to port %d...\n", POKER_PORT);
    fflush(stdout);


    if (bind(server_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        fprintf(stderr, "[poker] Failed to bind socket to port %d\n", POKER_PORT);
        perror("bind");
        close(server_sock);
        exit(1);
    }

    printf("[poker] Socket bound to port %d\n", POKER_PORT);
    fflush(stdout);

    if (listen(server_sock, 10) == -1) {
        fprintf(stderr, "[poker] Failed to listen on port %d\n", POKER_PORT);
        perror("listen");
        close(server_sock);
        exit(1);
    }

    printf("[poker] Server listening on port %d...\n", POKER_PORT);
    fflush(stdout);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int* client_sock = malloc(sizeof(int));

        *client_sock = accept(server_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (*client_sock == -1) {
            fprintf(stdout, "[poker] Failed to accept connection\n");
            fflush(stdout);
            perror("accept");
            free(client_sock);
            continue;
        }

        printf("[poker] Accepted connection from %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        fflush(stdout);

        pthread_t thread;
        client_info *client = malloc(sizeof(client_info));
        client->pgid = child_pgid;
        client->sockfd = *client_sock;
        if (pthread_create(&thread, NULL, handle_client, client) != 0) {
            fprintf(stderr, "[poker] Failed to create thread for client\n");
            perror("pthread_create");
            close(*client_sock);
            free(client_sock);
        }
        pthread_detach(thread);  // Clean up thread resources automatically
    }

    close(server_sock);
    exit(0);
}

int connect_with_timeout(int sockfd, const struct sockaddr *addr, socklen_t addrlen) {
    int flags, rc;
    fd_set writefds;
    struct timeval tv;

    // Get current socket flags
    if ((flags = fcntl(sockfd, F_GETFL, 0)) == -1) {
        fprintf(stdout, "[poker] fcntl(F_GETFL) error: %s\n", strerror(errno));
        return -1;
    }

    // Set socket to non-blocking
    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) == -1) {
        fprintf(stdout, "[poker] fcntl(F_SETFL) error: %s\n", strerror(errno));
        return -1;
    }

    // Initiate connection
    rc = connect(sockfd, addr, addrlen);
    if (rc == 0) {
        // Connection completed immediately
        fcntl(sockfd, F_SETFL, flags); // Restore original flags
        return 0;
    }

    if (errno != EINPROGRESS) {
        fprintf(stdout, "[poker] connect error: %s\n", strerror(errno));
        return -1;
    }

    // Wait for connection completion with timeout
    FD_ZERO(&writefds);
    FD_SET(sockfd, &writefds);
    tv.tv_sec = 1; // Timeout in seconds
    tv.tv_usec = 0;

    rc = select(sockfd + 1, NULL, &writefds, NULL, &tv);
    if (rc == 0) {
        // Timeout occurred
        errno = ETIMEDOUT;
        fprintf(stdout, "[poker] connection timeout\n");
        return -1;
    } else if (rc < 0) {
        fprintf(stdout, "[poker] select error: %s\n", strerror(errno));
        return -1;
    }

    // Check socket error status
    int error = 0;
    socklen_t len = sizeof(error);
    if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
        fprintf(stdout, "[poker] getsockopt error: %s\n", strerror(errno));
        return -1;
    }

    if (error != 0) {
        errno = error;
        fprintf(stdout, "[poker] socket error: %s\n", strerror(errno));
        return -1;
    }

    // Connection successful
    fcntl(sockfd, F_SETFL, flags); // Restore original flags
    return 0;
}

int connect_to_service(const char *service_name) {
    char hostname[256];
    struct addrinfo hints, *res, *p;
    int sockfd = -1;
    int status;

    snprintf(hostname, sizeof(hostname), "%s.default.svc.cluster.local", service_name);

    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;    // Try both IPv4 and IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    fprintf(stdout, "[poker] Resolving %s...\n", hostname);
    fflush(stdout);

    if ((status = getaddrinfo(hostname, POKER_PORT_STR, &hints, &res)) != 0) {
        fprintf(stdout, "[poker] getaddrinfo error: %s\n", gai_strerror(status));
        fflush(stdout);
        return -1;
    }

    // Try each address until successful connection
    for (p = res; p != NULL; p = p->ai_next) {
        char ipstr[INET6_ADDRSTRLEN];
        void *addr;
        char *ipver;

        // Get pointer to address
        if (p->ai_family == AF_INET) { // IPv4
            struct sockaddr_in *ipv4 = (struct sockaddr_in *)p->ai_addr;
            addr = &(ipv4->sin_addr);
            ipver = "IPv4";
        } else { // IPv6
            struct sockaddr_in6 *ipv6 = (struct sockaddr_in6 *)p->ai_addr;
            addr = &(ipv6->sin6_addr);
            ipver = "IPv6";
        }

        // Convert IP to string
        inet_ntop(p->ai_family, addr, ipstr, sizeof ipstr);
        fprintf(stdout, "[poker] Attempting %s connection to %s (%s)...\n", 
               ipver, ipstr, hostname);
        fflush(stdout);

        if ((sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol)) == -1) {
            fprintf(stdout, "[poker] socket error: %s\n", strerror(errno));
            continue;
        }

        // Set TCP_NODELAY for low latency
        int flag = 1;
        setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

        if (connect_with_timeout(sockfd, p->ai_addr, p->ai_addrlen) == 0) {
            break; // Success
        }

        close(sockfd);
        sockfd = -1;
    }

    freeaddrinfo(res);

    if (sockfd == -1) {
        fprintf(stdout, "[poker] Failed to connect to %s\n", hostname);
        fflush(stdout);
        return -1;
    }

    fprintf(stdout, "[poker] Successfully connected to %s\n", hostname);
    fflush(stdout);
    return sockfd;
}

// int connect_to_service(const char *service_name) {
//     char hostname[256];
//     snprintf(hostname, sizeof(hostname), "%s.default.svc.cluster.local", service_name);
//     struct addrinfo hints, *res, *p;
//     int sockfd;

//     memset(&hints, 0, sizeof hints);
//     hints.ai_family = AF_INET;     // Use AF_INET or AF_INET6 to force version
//     hints.ai_socktype = SOCK_STREAM; // TCP

//     // Resolve hostname to IP
//     int status = getaddrinfo(hostname, POKER_PORT_STR, &hints, &res);
//     if (status != 0) {
//         // fprintf(stderr, "getaddrinfo error: %s\n", gai_strerror(status));
//         fprintf(stdout, "[poker] getaddrinfo error: %s\n", gai_strerror(status));
//         fflush(stdout);
//         return -1;
//     }

//     // Loop through results and connect to the first we can
//     for (p = res; p != NULL; p = p->ai_next) {
//         // Create socket
//         sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
//         if (sockfd == -1) continue;
//         // int flags = fcntl(sockfd, F_GETFL, 0);
//         // fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);

//         struct timeval timeout = {1, 0};
//         setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
//         // Try to connect
//         printf("[poker] Attempting to connect to %s\n", hostname);
//         fflush(stdout);

//         if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
//             fprintf(stdout, "connect error: %s\n", strerror(errno));
//             fflush(stdout);
//             close(sockfd);
//             continue;
//         }
//         // else {
//         //     fcntl(sockfd, F_SETFL, flags); 
//         // }

//         break; // If we get here, we successfully connected
//     }


//     if (p == NULL) {
//         fprintf(stderr, "Failed to connect\n");
//         return -1;
//     }

//     fprintf(stdout, "[poker] Connected to %s\n", hostname);
//     fflush(stdout);
//     freeaddrinfo(res); // Free the linked list
//     return sockfd;
// }

void register_neighbors() {
    // pthread_mutex_lock(&neighbor_lock);
    char *neighbors_env = getenv("SLOWPOKE_NEIGHBORS");
    if (!neighbors_env) {
        fprintf(stderr, "[poker] Environment variable SLOWPOKE_NEIGHBORS not found\n");
        fflush(stderr);
        return;
    }

    int count_services(const char *str) {
        if (!str || !*str) return 0;
        
        int count = 1;
        while (*str) {
            if (*str == ':') count++;
            str++;
        }
        return count;
    }

    num_neighbors = count_services(neighbors_env);
    if (num_neighbors == 0) {
        fprintf(stderr, "[poker] No services found in SLOWPOKE_NEIGHBORS\n");
        return;
    }

    fprintf(stdout, "[poker] Neighbors: %s\n", neighbors_env);
    fflush(stdout);

    neighbor_conns = malloc(num_neighbors * sizeof(int));

    // Parse and connect
    char *copy = strdup(neighbors_env);
    char *token = strtok(copy, ":");
    int current = 0;

    while (token != NULL && current < num_neighbors) {
        // Trim whitespace
        char *end = token + strlen(token) - 1;
        while (end > token && isspace((unsigned char)*end)) end--;
        end[1] = '\0';
        while (isspace((unsigned char)*token)) token++;
        if (*token != '\0') {
            fprintf(stdout, "[poker] Connecting to %s\n", token);
            fflush(stdout);
            int sockfd = connect_to_service(token);
            while (sockfd == -1) {
                // fprintf(stderr, "[poker] Failed to connect to %s, retrying...\n", token);
                fprintf(stdout, "[poker] Failed to connect to %s, retrying...\n", token);
                fflush(stdout);
                usleep(100000);  // Sleep for 100 ms before retrying
                sockfd = connect_to_service(token);
            }
            fprintf(stdout, "[poker] Connected to %s\n", token);
            neighbor_conns[current] = sockfd;
            current++;
        }
        token = strtok(NULL, ":");
    }

    fprintf(stdout, "[poker] Connected to %d neighbors\n", current);
    fflush(stdout);
    for (int i = 0; i < current; i++) {
        // Set TCP_NODELAY for low latency
        fprintf(stdout, "[poker] neighbor %d: %d\n", i, neighbor_conns[i]);
        fflush(stdout);
    }
    free(copy);
    // pthread_mutex_unlock(&neighbor_lock);
}

// Function for the FIFO monitoring thread
void *poker(void *arg) {

    pid_t child_pgid = *((pid_t *)arg);

    // start the tcp server in a thread
    pthread_t tcp_thread;
    if (pthread_create(&tcp_thread, NULL, poker_server, &child_pgid) != 0) {
        fprintf(stderr, "[poker] Failed to create TCP server thread\n");
        perror("pthread_create (tcp_thread)");
        pthread_exit(NULL);
    }

    // open the tcp server for all neighbors
    register_neighbors();

    int fifo_fd = open(FIFO_PATH, O_RDONLY | O_NONBLOCK);
    int is_blocking = 0;
    char buffer[256];
    if (fifo_fd == -1) {
        perror("open");
        pthread_exit(NULL);
    }

    char send_buf[8];

    while (1) {
        // Attempt to read from the FIFO
        ssize_t bytes_read = read(fifo_fd, buffer, sizeof(buffer) - 1);
        if (bytes_read == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            usleep(100000);
        } else if (bytes_read == 0) {
            // EOF (other end closed)
            usleep(100000);
        } else {
            // Null-terminate the message and print it
            if (!is_blocking) {
                is_blocking = 1;
                int flags = fcntl(fifo_fd, F_GETFL, 0);
                if (flags == -1) {
                    perror("fcntl(F_GETFL)");
                    pthread_exit(NULL);
                }
                if (fcntl(fifo_fd, F_SETFL, flags & ~O_NONBLOCK) == -1) {
                    perror("fcntl(F_SETFL)");
                    pthread_exit(NULL);
                }
                continue;
            }
            
            int64_t nanosleep = *(int64_t *)(&buffer[0]);
            if (nanosleep <= 0) {
                continue;
            }
            pthread_mutex_lock(&poker_phase_lock);
            poker_phase += 1;
            poker_message msg = {poker_phase, nanosleep};
            if (DEBUG) {
                printf("[poker] Received counter: phase=%lld, delay_nanos=%lld\n", msg.phase, msg.delay_nanos);
                fflush(stdout);
            }
            pthread_mutex_unlock(&poker_phase_lock);
            propagate_poker_message_to_neighbors(&msg);
        }

        // Check if the child has exited
        if (child_exited) {
            break;
        }
    }

    close(fifo_fd);
    pthread_exit(NULL);
}