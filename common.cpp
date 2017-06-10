#include "common.h"

#include <sys/epoll.h>
#include <unistd.h>

void add_event(int epollfd, int fd, int events) {
    struct epoll_event event;
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event) < 0) {
        perror("[FATAL] add_event()");
    }
}

void modify_event(int epollfd, int fd, int events) {
    struct epoll_event event;
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event) < 0) {
        perror("[FATAL] modify_event()");
    }
}

void delete_event(int epollfd, int fd) {
    if (epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL) < 0) {
        perror("[FATAL] delete_event()");
    }
}

// TODO: handle disconnection properly
bool handle_read_common(int clientfd, Buffer* buf) {
    size_t* req_len = (size_t*)buf->get_rptr(0); // the value pointed to may not be valid

    // Read the request length if we have not got it yet
    if (buf->size() < sizeof(size_t)) {
        ssize_t len = buf->input_from_fd(clientfd);
        if (len == 0) {
            close(clientfd);
            return false;
        } else if (len < 0) {
            return false;
        }
        // If we have got the request length, set the buffer length properly and read the request
        if (buf->size() >= sizeof(size_t)) {
            buf->reserve(*req_len); // req_len now points to a valid value
            req_len = (size_t*)buf->get_rptr(0); // the memory location may change after changing the internal buffer size
            if (buf->size() < *req_len) {
                len = buf->input_from_fd(clientfd);
                if (len == 0) {
                    close(clientfd);
                    return false;
                } else if (len < 0) {
                    return false;
                }
            }
        } else {
            return false;
        }
    // Read the request 
    } else if (buf->size() < *req_len) {
        ssize_t len = buf->input_from_fd(clientfd);
        if (len == 0) {
            close(clientfd);
            return false;
        } else if (len < 0) {
            return false;
        }
    }

    return (buf->size() == *req_len);
}

