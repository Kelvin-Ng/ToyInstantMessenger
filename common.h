#include <unistd.h>

#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <cassert>

#define REQ_CS_REGISTER        1
#define REQ_CS_SEND_MSG        2
#define REQ_SC_NEW_MSG         3
#define REQ_SC_REGISTER_ACK    4

void add_event(int epollfd, int fd, int events);
void modify_event(int epollfd, int fd, int events);
void delete_event(int epollfd, int fd);

// TODO: allow pruning read segment
// TODO: better API for adding a new request (e.g. no need to compute the request length by the user)
class Buffer {
 public:
    Buffer() : wpos(0), rpos(0) {}
    explicit Buffer(size_t init_reserve) : buf_(init_reserve), wpos(0), rpos(0) {}

    inline void reserve(size_t len) {
        buf_.resize(len);
    }

    inline void enlarge(size_t len) {
        buf_.resize(buf_.size() + len);
    }

    inline void reset(int len) {
        reserve(len);
        wpos = 0;
        rpos = 0;
    }

    void write(const std::string& str) {
        size_t size = str.size();
        write(size);
        std::copy(str.begin(), str.end(), buf_.begin() + wpos);
        wpos += str.size();
    }

    template <typename T>
    void write(const T& ptr) {
        std::copy((char*)&ptr, ((char*)&ptr) + sizeof(T), buf_.begin() + wpos);
        wpos += sizeof(T);
    }

    template <typename T>
    const T* read() {
        const T* res = (const T*)&(buf_[rpos]);
        rpos += sizeof(T);
        return res;
    }

    std::string get_string() {
        const size_t* len = read<size_t>();
        std::string str((const char*)&(buf_[rpos]), *len);
        rpos += *len;
        return str;
    }

    inline const void* get_rptr(size_t pos) const {
        return &buf_[pos];
    }

    inline void* get_rptr(size_t pos) {
        return &buf_[pos];
    }

    inline void* get_wptr(size_t pos) {
        return &buf_[pos];
    }

    inline const void* get_rptr() const {
        return &buf_[rpos];
    }

    inline void* get_rptr() {
        return &buf_[rpos];
    }

    inline void* get_wptr() {
        return &buf_[wpos];
    }

    inline void inc_wpos(size_t inc) {
        wpos += inc;
    }

    inline void inc_rpos(size_t inc) {
        rpos += inc;
    }

    inline size_t get_wpos() const {
        return wpos;
    }

    inline size_t get_rpos() const {
        return rpos;
    }

    inline size_t size() const {
        return wpos;
    }

    inline size_t capacity() const {
        return buf_.size();
    }

    inline size_t remaining() const {
        return wpos - rpos;
    }

    inline ssize_t input_from_fd(int fd) {
        ssize_t len = ::read(fd, get_wptr(), capacity() - wpos);
        if (len > 0) {
            wpos += len;
        }
        return len;
    }

    inline ssize_t output_to_fd(int fd) {
        ssize_t len = ::write(fd, get_rptr(), size() - rpos);
        if (len > 0) {
            rpos += len;
        }
        assert(rpos <= size());
        return len;
    }

 private:
    std::vector<unsigned char> buf_;
    size_t wpos, rpos;
};

using UsernameFd = std::unordered_map<std::string, int>;
using FdUsername = std::unordered_map<int, std::string>;
using FdBuffer = std::unordered_map<int, Buffer>;

bool handle_read_common(int clientfd, Buffer* buf);

