#include <unistd.h>

#include <cstddef>
#include <string>
#include <vector>
#include <unordered_map>
#include <cassert>
#include <queue>
#include <memory>

#define REQ_CS_REGISTER         1
#define REQ_CS_SEND_MSG         2
#define REQ_SC_NEW_MSG          3
#define REQ_SC_REGISTER_ACK     4
#define REQ_CS_SEND_FILE        5
#define REQ_SC_NEW_FILE         6

void add_event(int epollfd, int fd, int events);
void modify_event(int epollfd, int fd, int events);
void delete_event(int epollfd, int fd);

// TODO: allow pruning read segment
// TODO: better API for adding a new request (e.g. no need to compute the request length by the user)
class Buffer {
 public:
    Buffer() : wpos_(0), rpos_(0) {}
    explicit Buffer(size_t init_reserve) : buf_(init_reserve), wpos_(0), rpos_(0) {}

    inline void reserve(size_t len) {
        buf_.resize(len);
    }

    inline void enlarge(size_t len) {
        buf_.resize(buf_.size() + len);
    }

    inline void reset(int len) {
        reserve(len);
        wpos_ = 0;
        rpos_ = 0;
    }

    void write(const std::string& str) {
        size_t size = str.size();
        write(size);
        std::copy(str.begin(), str.end(), buf_.begin() + wpos_);
        wpos_ += str.size();
    }

    template <typename T>
    void write(const T& ptr) {
        std::copy((char*)&ptr, ((char*)&ptr) + sizeof(T), buf_.begin() + wpos_);
        wpos_ += sizeof(T);
    }

    template <typename T>
    const T* read() {
        const T* res = (const T*)&(buf_[rpos_]);
        rpos_ += sizeof(T);
        return res;
    }

    std::string get_string() {
        const size_t* len = read<size_t>();
        std::string str((const char*)&(buf_[rpos_]), *len);
        rpos_ += *len;
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
        return &buf_[rpos_];
    }

    inline void* get_rptr() {
        return &buf_[rpos_];
    }

    inline void* get_wptr() {
        return &buf_[wpos_];
    }

    inline void inc_wpos(size_t inc) {
        wpos_ += inc;
    }

    inline void inc_rpos(size_t inc) {
        rpos_ += inc;
    }

    inline size_t get_wpos() const {
        return wpos_;
    }

    inline size_t get_rpos() const {
        return rpos_;
    }

    inline size_t size() const {
        return wpos_;
    }

    inline size_t capacity() const {
        return buf_.size();
    }

    inline size_t remaining() const {
        return wpos_ - rpos_;
    }

    inline bool empty() const {
        return wpos_ == rpos_;
    }

    inline ssize_t input_from_fd(int fd) {
        ssize_t len = ::read(fd, get_wptr(), capacity() - wpos_);
        if (len > 0) {
            wpos_ += len;
        }
        return len;
    }

    inline ssize_t output_to_fd(int fd) {
        ssize_t len = ::write(fd, get_rptr(), size() - rpos_);
        if (len > 0) {
            rpos_ += len;
        }
        assert(rpos_ <= size());
        return len;
    }

 private:
    std::vector<char> buf_;
    size_t wpos_, rpos_;
};

class BlockBuffer {
 public:
    BlockBuffer() : rpos_(0) {
        block_size_ = sysconf(_SC_PAGESIZE);
        wpos_ = block_size_;
    }

    BlockBuffer(ssize_t block_size) : rpos_(0) {
        if (block_size == -1) {
            block_size_ = sysconf(_SC_PAGESIZE);
        } else {
            block_size_ = block_size;
        }
        wpos_ = block_size_;
    }

    void write(const char* write_start, const char* write_end) {
        //// TODO: Probably an optimization for branch prediction
        //if (write_start >= write_end) {
        //    return;
        //}
        while (write_start < write_end) {
            if (wpos_ == block_size_) {
                if (free_list_.empty()) {
                    buf_.emplace(new char[block_size_]);
                } else {
                    buf_.push(std::move(free_list_.back()));
                    free_list_.pop_back();
                }
                wpos_ = 0;
            }

            size_t to_write = std::min((size_t)(write_end - write_start), block_size_ - wpos_);
            std::copy(write_start, write_start + to_write, buf_.back().get() + wpos_);
            write_start += to_write;
            wpos_ += to_write;
        }
    }

    template <typename T>
    inline void write(const T& ptr) {
        write((const char*)&ptr, (const char*)&ptr + sizeof(T));
    }

    void write(const std::string& str) {
        size_t size = str.size();
        write(size);
        write(str.c_str(), str.c_str() + size);
    }

    inline ssize_t output_to_fd(int fd) {
        if (buf_.empty()) {
            return 0;
        }

        ssize_t total_len = 0;

        for (;;) {
            ssize_t len;
            if (buf_.size() == 1) {
                if (rpos_ == wpos_) {
                    break;
                }
                len = ::write(fd, buf_.front().get() + rpos_, wpos_ - rpos_);
            } else {
                len = ::write(fd, buf_.front().get() + rpos_, block_size_ - rpos_);
            }
            if (len < 0) {
                if (total_len == 0) {
                    return len;
                } else {
                    break;
                }
            } else if (len == 0) {
                break;
            }
            rpos_ += len;
            total_len += len;

            if (rpos_ == block_size_) {
                // TODO: May want to actually free the memory
                free_list_.push_back(std::move(buf_.front()));
                buf_.pop();
                rpos_ = 0;

                if (buf_.empty()) {
                    break;
                }
            }
        }

        return total_len;
    }

    inline bool empty() const {
        return buf_.empty() || (buf_.size() == 1 && rpos_ == wpos_);
    }

 private:
    size_t block_size_;
    std::queue<std::unique_ptr<char[]>> buf_;
    std::vector<std::unique_ptr<char[]>> free_list_;
    size_t wpos_, rpos_;
};

using UsernameFd = std::unordered_map<std::string, int>;
using FdUsername = std::unordered_map<int, std::string>;
using FdBuffer = std::unordered_map<int, Buffer>;
using FdBlockBuffer = std::unordered_map<int, BlockBuffer>;

bool handle_read_common(int clientfd, Buffer* buf);

