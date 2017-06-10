/*
 * TODO list:
 * The TODO in the code
 * Handle client disconnections
 */

#include "common.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cassert>

#define LISTENQ         5
#define EPOLLEVENTS     100

int socket_bind(const char* ip_addr, int port) {
    int listenfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);

    struct sockaddr_in addr;
    bzero(&addr, sizeof(addr));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, ip_addr, &addr.sin_addr);
    addr.sin_port = htons(port);
    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("[FATAL] bind()");
        exit(1);
    }

    return listenfd;
}

void handle_accpet(int epollfd, int listenfd) {
    struct sockaddr_in addr;
    socklen_t addr_len;
    int clientfd = accept4(listenfd, (struct sockaddr*)&addr, &addr_len, SOCK_NONBLOCK);
    if (clientfd == -1) {
        perror("[WARN] accept4()");
    }

    fprintf(stderr, "[INFO] Accepted new user\n");

    add_event(epollfd, clientfd, EPOLLIN);
}

void handle_register(int epollfd, int clientfd, UsernameFd* username_fd, FdUsername* fd_username, Buffer* buf, FdBuffer* fd_buffer_out) {
    std::string username = buf->get_string();

    if (username.size() > 0) {
        (*username_fd)[username] = clientfd;
        (*fd_username)[clientfd] = username;

        size_t req_len = sizeof(size_t) + sizeof(int);
        Buffer* buf_out = &fd_buffer_out->emplace(clientfd, req_len).first->second;
        buf_out->write(req_len);
        buf_out->write(REQ_SC_REGISTER_ACK);
        modify_event(epollfd, clientfd, EPOLLIN | EPOLLOUT);

        fprintf(stderr, "[INFO] New user registered: %s\n", username.c_str());
    } else {
        fprintf(stderr, "[ERROR] The user does not send the username\n");
        return;
    }
}

void handle_msg_send(int epollfd, int senderfd, const UsernameFd& username_fd, const FdUsername& fd_username, Buffer* buf, FdBuffer* fd_buffer_out) {
    // TODO: reduce copying, probably need string_view?
    const std::string& sender = fd_username.find(senderfd)->second;
    size_t sender_size = sender.size();
    std::string recver = buf->get_string();
    std::string msg = buf->get_string();
    size_t msg_size = msg.size();
    int recverfd = username_fd.find(recver)->second; // TODO: handle non-existing receiver

    size_t req_size = sender_size + msg_size + 3 * sizeof(size_t) + sizeof(int);
    Buffer* buf_out;
    bool has_remaining;

    auto it = fd_buffer_out->find(recverfd);
    if (it == fd_buffer_out->end()) {
        buf_out = &fd_buffer_out->emplace(recverfd, req_size).first->second;
        has_remaining = false;
    } else {
        buf_out = &it->second;
        has_remaining = buf_out->remaining();
        buf_out->enlarge(req_size);
    }
    buf_out->write(req_size);
    buf_out->write(REQ_SC_NEW_MSG);
    buf_out->write(sender);
    buf_out->write(msg);

    if (!has_remaining) {
        // TODO: try writing before polling
        modify_event(epollfd, recverfd, EPOLLIN | EPOLLOUT);
    }
}

// TODO: Should I put all requests into one single buffer?
void handle_read(int epollfd, int clientfd, FdBuffer* fd_buffer, UsernameFd* username_fd, FdUsername* fd_username, FdBuffer* fd_buffer_out) {
    // Get the corresponding buffer
    auto it = fd_buffer->find(clientfd);
    // If the buffer does not exist, create one
    if (it == fd_buffer->end()) {
        it = fd_buffer->emplace(clientfd, sizeof(size_t)).first;
    }

    Buffer* buf = &it->second;

    // Read the request. If we have finished reading the request, process it
    if (handle_read_common(clientfd, buf)) {
        buf->inc_rpos(sizeof(size_t));
        const int* req_type = buf->read<int>();

        switch (*req_type) {
            case REQ_CS_REGISTER:
                handle_register(epollfd, clientfd, username_fd, fd_username, buf, fd_buffer_out);
                break;
            case REQ_CS_SEND_MSG:
                handle_msg_send(epollfd, clientfd, *username_fd, *fd_username, buf, fd_buffer_out);
                break;
        }

        fd_buffer->erase(it);
    }
}

void handle_write(int epollfd, int recverfd, FdBuffer* fd_buffer_out) {
    Buffer* buf_out = &fd_buffer_out->find(recverfd)->second;

    if (buf_out->output_to_fd(recverfd) < 0) {
        return;
    }
 
    // TODO: If we use edge trigger, we can avoid modifying the event frequently
    if (!buf_out->remaining()) {
        modify_event(epollfd, recverfd, EPOLLIN);
        fd_buffer_out->erase(recverfd);
    }
}

int main(int argc, char** argv) {
    if (argc != 3) {
        printf("Usage: ./server <ip_addr> <port>\n");
    }

    int listenfd = socket_bind(argv[1], atoi(argv[2]));
    listen(listenfd, LISTENQ);

    int epollfd = epoll_create(1);
    add_event(epollfd, listenfd, EPOLLIN);

    struct epoll_event events[EPOLLEVENTS];

    UsernameFd username_fd;
    FdUsername fd_username;
    FdBuffer fd_buffer;
    FdBuffer fd_buffer_out;

    for (;;) {
        int num = epoll_wait(epollfd, events, EPOLLEVENTS, -1);

        for (int i = 0; i < num; ++i) {
            int fd = events[i].data.fd;

            if (fd == listenfd) {
                if (events[i].events & EPOLLIN) {
                    handle_accpet(epollfd, listenfd);
                }
            } else {
                if (events[i].events & EPOLLIN) {
                    handle_read(epollfd, fd, &fd_buffer, &username_fd, &fd_username, &fd_buffer_out);
                }

                if (events[i].events & EPOLLOUT) {
                    handle_write(epollfd, fd, &fd_buffer_out);
                }
            }
        }
    }
}

