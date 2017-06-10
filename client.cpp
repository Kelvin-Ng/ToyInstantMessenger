#include "common.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <cstdio>
#include <cstring>

#define EPOLLEVENTS     100

int socket_connect(const char* ip_addr, int port) {
    int sockfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr, &addr.sin_addr);
    addr.sin_port = htons(port);
    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        if (errno != EINPROGRESS) {
            perror("[FATAL] connect()");
            exit(1);
        }
    }

    return sockfd;
}

void req_register(const std::string& username, Buffer* buf_out) {
    size_t req_len = 2 * sizeof(size_t) + sizeof(int) + username.size();
    buf_out->enlarge(req_len);
    buf_out->write(req_len);
    buf_out->write(REQ_CS_REGISTER);
    buf_out->write(username);
}

void print_new_msg(Buffer* buf) {
    std::string sender = buf->get_string();
    std::string msg = buf->get_string();

    printf("%s says: %s\n", sender.c_str(), msg.c_str());
}

// TODO: Should I put all requests into one single buffer? It seems that there is no need
void handle_read(int epollfd, int sockfd, Buffer* buf) {
    // Read the request. If we have finished reading the request, process it
    if (handle_read_common(sockfd, buf)) {
        buf->inc_rpos(sizeof(size_t));
        const int* req_type = buf->read<int>();

        switch (*req_type) {
            case REQ_SC_REGISTER_ACK:
                fprintf(stderr, "[INFO] Registered successfully\n");
                add_event(epollfd, STDIN_FILENO, EPOLLIN);
                break;
            case REQ_SC_NEW_MSG:
                print_new_msg(buf);
                break;
        }

        buf->reset(sizeof(size_t));
    }
}

void handle_write(int epollfd, int sockfd, Buffer* buf_out) {
    if (buf_out->output_to_fd(sockfd) >= 0) {
        if (!buf_out->remaining()) {
            modify_event(epollfd, sockfd, EPOLLIN);
        }
    }
}

void handle_stdin(int epollfd, int sockfd, Buffer* buf_out) {
    bool has_remaining = buf_out->remaining();

    // TODO: reduce copying
    ssize_t len;
    char buf_[255];
    std::string raw_msg;
    while ((len = read(STDIN_FILENO, buf_, 256)) > 0) {
        if (buf_[len - 1] == '\n') {
            buf_[len - 1] = 0;
            raw_msg += buf_;

            size_t colon_pos = raw_msg.find(":");
            std::string recver = raw_msg.substr(0, colon_pos);
            std::string msg = raw_msg.substr(colon_pos + 2);

            size_t req_len = recver.size() + msg.size() + 3 * sizeof(size_t) + sizeof(int);
            buf_out->enlarge(req_len);
            buf_out->write(req_len);
            buf_out->write(REQ_CS_SEND_MSG);
            buf_out->write(recver);
            buf_out->write(msg);
        } else {
            buf_[len] = 0;
            raw_msg += buf_;
        }
    }

    if (!has_remaining && buf_out->remaining()) {
        modify_event(epollfd, sockfd, EPOLLIN | EPOLLOUT);
    }
}

int main(int argc, char** argv) {
    if (argc != 4) {
        printf("Usage: ./client <ip_addr> <port> <username>\n");
    }

    int sockfd = socket_connect(argv[1], atoi(argv[2]));

    int epollfd = epoll_create(1);
    add_event(epollfd, sockfd, EPOLLIN | EPOLLOUT);

    struct epoll_event events[EPOLLEVENTS];

    int has_connect_error = -1;

    Buffer buf(sizeof(size_t));
    Buffer buf_out;

    req_register(argv[3], &buf_out);

    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
    
    for (;;) {
        int num = epoll_wait(epollfd, events, EPOLLEVENTS, -1);
        for (int i = 0; i < num; ++i) {
            int fd = events[i].data.fd;
            if (fd == sockfd) {
                if (events[i].events & EPOLLIN) {
                    handle_read(epollfd, sockfd, &buf);
                }

                if (events[i].events & EPOLLOUT) {
                    if (has_connect_error == -1) {
                        socklen_t optlen = sizeof(int);
                        if (getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &has_connect_error, &optlen) < 0) {
                            perror("[FATAL] getsockopt()");
                            exit(1);
                        } else if (has_connect_error != 0) {
                            perror("[FATAL] connect()");
                            exit(1);
                        } else {
                            fprintf(stdout, "[INFO] Connected to server successfully\n");
                        }
                    }
                    handle_write(epollfd, sockfd, &buf_out);
                }
            } else if (fd == STDIN_FILENO && (events[i].events & EPOLLIN)) {
                handle_stdin(epollfd, sockfd, &buf_out);
            }
        }
    }
}

