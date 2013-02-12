/**
 * @file
 * @brief Utilities.
 * @author HOSHINO Takashi
 *
 * (C) 2012 Cybozu Labs, Inc.
 */
#ifndef UTIL_HPP
#define UTIL_HPP

#include <cassert>
#include <stdexcept>
#include <sstream>
#include <cstdarg>
#include <algorithm>
#include <vector>
#include <queue>
#include <mutex>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <cstdint>
#include <cinttypes>
#include <string>
#include <cerrno>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#define RT_ERR(fmt, args...)                                    \
    std::runtime_error(walb::util::formatString(fmt, ##args))

#define CHECKx(cond)                                                \
    do {                                                            \
        if (!(cond)) {                                              \
            throw RT_ERR("check error: %s:%d", __func__, __LINE__); \
        }                                                           \
    } while (0)

namespace walb {
namespace util {

/**
 * Create a std::string using printf() like formatting.
 */
std::string formatString(const char * format, ...)
{
    char *p = nullptr;
    va_list args;
    va_start(args, format);
    int ret = ::vasprintf(&p, format, args);
    va_end(args);
    if (ret < 0) {
        ::free(p);
        throw std::runtime_error("vasprintf failed.");
    }
    std::string st(p, ret);
    ::free(p);
    return st;
}

/**
 * Formst string with va_list.
 */
std::string formatStringV(const char *format, va_list ap)
{
    char *p = nullptr;
    int ret = ::vasprintf(&p, format, ap);
    if (ret < 0) {
        ::free(p);
        throw std::runtime_error("vasprintf failed.");
    }
    std::string st(p, ret);
    ::free(p);
    return st;
}

/**
 * formatString() test.
 */
void testFormatString()
{
    {
        std::string st(formatString("%s%c%s", "012", (char)0, "345"));
        for (size_t i = 0; i < st.size(); i++) {
            printf("%0x ", st[i]);
        }
        printf("\n size %zu\n", st.size());
        assert(st.size() == 7);
    }

    {
        std::string st(formatString(""));
        ::printf("%s %zu\n", st.c_str(), st.size());
    }

    {
        try {
            std::string st(formatString(nullptr));
            assert(false);
        } catch (std::runtime_error& e) {
        }
    }

    {
        std::string st(formatString("%s%s", "0123456789", "0123456789"));
        ::printf("%s %zu\n", st.c_str(), st.size());
        assert(st.size() == 20);
    }
}

static inline double getTime()
{
    struct timeval tv;
    double t;

    ::gettimeofday(&tv, NULL);

    t = static_cast<double>(tv.tv_sec) +
        static_cast<double>(tv.tv_usec) / 1000000.0;
    return t;
}

/**
 * Libc error wrapper.
 */
class LibcError : public std::exception {
public:
    LibcError() : LibcError(errno) {}

    LibcError(int errnum, const char* msg = "libc_error: ")
        : errnum_(errnum)
        , str_(formatString("%s%s", msg, ::strerror(errnum))) {}

    virtual const char *what() const noexcept {
        return str_.c_str();
    }
private:
    int errnum_;
    std::string str_;
};

/**
 * Eof error for IO.
 */
class EofError : public std::exception {
public:
    virtual const char *what() const noexcept {
        return "eof error";
    }
};

/**
 * File descriptor operations wrapper.
 */
class FdOperator
{
private:
    int fd_;
public:
    FdOperator(int fd)
        : fd_(fd) {}

    /**
     * read.
     */
    void read(char *buf, size_t size) {
        size_t s = 0;
        while (s < size) {
            ssize_t ret = ::read(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw LibcError(errno, "read failed: ");
            }
            if (ret == 0) {
                throw EofError();
            }
            s += ret;
        }
    }

    /**
     * write.
     */
    void write(const char *buf, size_t size) {
        size_t s = 0;
        while (s < size) {
            ssize_t ret = ::write(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw LibcError(errno, "write failed: ");
            }
            if (ret == 0) {
                throw EofError();
            }
            s += ret;
        }
    }

    /**
     * lseek.
     */
    void lseek(off_t oft, int whence) {
        off_t ret = ::lseek(fd_, oft, whence);
        if (ret == -1) {
            throw LibcError(errno, "lseek failed: ");
        }
    }

    /**
     * fdatasync.
     */
    void fdatasync() {
        int ret = ::fdatasync(fd_);
        if (ret) {
            throw LibcError(errno, "fdsync failed: ");
        }
    }

    /**
     * fsync.
     */
    void fsync() {
        int ret = ::fsync(fd_);
        if (ret) {
            throw LibcError(errno, "fsync failed: ");
        }
    }
};

/**
 * File descriptor wrapper. Read only.
 */
class FdReader : public FdOperator
{
public:
    FdReader(int fd) : FdOperator(fd) {}
    void write(const char *buf, size_t size) = delete;
    void fdatasync() = delete;
    void fsync() = delete;
};

/**
 * File descriptor wrapper. Write only.
 */
class FdWriter : public FdOperator
{
public:
    FdWriter(int fd) : FdOperator(fd) {}
    virtual void read(char *buf, size_t size) = delete;
};

/**
 * A simple file opener.
 * close() will be called in the destructor when you forget to call it.
 */
class FileOpener
{
private:
    int fd_;
    std::once_flag closeFlag_;
public:
    FileOpener(const std::string& filePath, int flags)
        : fd_(staticOpen(filePath, flags))
        , closeFlag_() {}

    FileOpener(const std::string& filePath, int flags, int mode)
        : fd_(staticOpen(filePath, flags, mode))
        , closeFlag_() {}

    ~FileOpener() {
        try {
            close();
        } catch (...) {
        }
    }

    int fd() const {
        if (fd_ < 0) {
            throw RT_ERR("fd < 0.");
        }
        return fd_;
    }

    void close() {
        std::call_once(closeFlag_, [&]() {
                if (::close(fd_)) {
                    throw LibcError(errno, "close failed: ");
                }
                fd_ = -1;
            });
    }
private:
    static int staticOpen(const std::string& filePath, int flags) {
        int fd = ::open(filePath.c_str(), flags);
        if (fd < 0) {
            throw LibcError(errno, "open failed: ");
        }
        return fd;
    }
    static int staticOpen(const std::string& filePath, int flags, int mode) {
        int fd = ::open(filePath.c_str(), flags, mode);
        if (fd < 0) {
            throw LibcError(errno, "open failed: ");
        }
        return fd;
    }
};

class BlockDevice
{
private:
    std::string name_;
    int openFlags_;
    int fd_;
    bool isBlockDevice_;
    uint64_t deviceSize_; // [bytes].
    unsigned int lbs_; // logical block size [bytes].
    unsigned int pbs_; // physical block size [bytes].

    std::once_flag closeFlag_;

public:
    BlockDevice(const std::string& name, int flags)
        : name_(name)
        , openFlags_(flags)
        , fd_(openDevice(name, flags))
        , isBlockDevice_(isBlockDeviceStatic(fd_))
        , deviceSize_(getDeviceSizeStatic(fd_))
        , lbs_(getLogicalBlockSizeStatic(fd_))
        , pbs_(getPhysicalBlockSizeStatic(fd_)) {
#if 0
        ::printf("device %s size %zu isWrite %d isDirect %d isBlockDevice %d "
                 "lbs %u pbs %u\n",
                 name_.c_str(), deviceSize_,
                 (openFlags_ & O_RDWR) != 0, (openFlags_ & O_DIRECT) != 0,
                 isBlockDevice_, lbs_, pbs_);
#endif
    }

    explicit BlockDevice(BlockDevice&& rhs)
        : name_(std::move(rhs.name_))
        , openFlags_(rhs.openFlags_)
        , fd_(rhs.fd_)
        , isBlockDevice_(rhs.isBlockDevice_)
        , deviceSize_(rhs.deviceSize_)
        , lbs_(rhs.lbs_)
        , pbs_(rhs.pbs_) {

        rhs.fd_ = -1;
    }

    BlockDevice& operator=(BlockDevice&& rhs) {
        name_ = std::move(rhs.name_);
        openFlags_ = rhs.openFlags_;
        fd_ = rhs.fd_; rhs.fd_ = -1;
        isBlockDevice_ = rhs.isBlockDevice_;
        deviceSize_= rhs.deviceSize_;
        lbs_ = rhs.lbs_;
        pbs_ = rhs.pbs_;
        return *this;
    }

    ~BlockDevice() {
        try {
            close();
        } catch (...) {
        }
    }

    void close() {
        std::call_once(closeFlag_, [&]() {
                if (fd_ > 0) {
                    if (::close(fd_) < 0) {
                        throw LibcError(errno, "close failed: ");
                    }
                    fd_ = -1;
                }
            });
    }

    /**
     * Read data and fill a buffer.
     */
    void read(off_t oft, size_t size, char* buf) {
        if (deviceSize_ < oft + size) { throw EofError(); }
        ::lseek(fd_, oft, SEEK_SET);
        size_t s = 0;
        while (s < size) {
#if 0
            ::fprintf(::stderr, "read %d %p &buf[%zu], %zu\n", fd_, &buf[s], s, size - s);
#endif
            ssize_t ret = ::read(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw LibcError(errno, "read failed: ");
            }
            if (ret == 0) {
                throw EofError();
            }
            s += ret;
        }
    }

    /**
     * Write data of a buffer.
     */
    void write(off_t oft, size_t size, const char* buf) {
        if (deviceSize_ < oft + size) { throw EofError(); }
        ::lseek(fd_, oft, SEEK_SET);
        size_t s = 0;
        while (s < size) {
#if 0
            ::fprintf(::stderr, "write %d %p &buf[%zu], %zu\n", fd_, &buf[s], s, size - s);
#endif
            ssize_t ret = ::write(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw LibcError(errno, "write failed: ");
            }
            if (ret == 0) {
                throw EofError();
            }
            s += ret;
        }
    }

    /**
     * fdatasync.
     */
    void fdatasync() {
        int ret = ::fdatasync(fd_);
        if (ret) {
            throw LibcError(errno, "fdatasync failed: ");
        }
    }

    /**
     * fsync.
     */
    void fsync() {
        int ret = ::fsync(fd_);
        if (ret) {
            throw LibcError(errno, "fsync failed: ");
        }
    }

    /**
     * Get device size [byte].
     */
    uint64_t getDeviceSize() const { return deviceSize_; }

    /**
     * Open flags.
     */
    int getFlags() const { return openFlags_; }

    /**
     * File descriptor.
     */
    int getFd() const { return fd_; }

    /**
     * RETURN:
     *   True if the descriptor is of a block device file,
     *   or false.
     */
    bool isBlockDevice() const { return isBlockDevice_; }

    unsigned int getPhysicalBlockSize() const { return pbs_; }
    unsigned int getLogicalBlockSize() const { return lbs_; }

private:
    /**
     * Helper function for constructor.
     */
    static int openDevice(const std::string& name, int flags) {
        int fd = ::open(name.c_str(), flags);
        if (fd < 0) {
            throw LibcError(
                errno, formatString("open %s failed: ", name.c_str()).c_str());
        }
        return fd;
    }

    static void statStatic(int fd, struct stat *s) {
        assert(fd >= 0);
        assert(s);
        if (::fstat(fd, s) < 0) {
            throw LibcError(errno, "fstat failed: ");
        }
    }

    static unsigned int getPhysicalBlockSizeStatic(int fd) {
        assert(fd >= 0);

        if (!isBlockDeviceStatic(fd)) {
            return 512;
        }

        unsigned int pbs;
        if (::ioctl(fd, BLKPBSZGET, &pbs) < 0) {
            throw LibcError(errno, "Getting physical block size failed: ");
        }
        assert(pbs > 0);
        return pbs;
    }

    static unsigned int getLogicalBlockSizeStatic(int fd) {
        assert(fd >= 0);

        if (!isBlockDeviceStatic(fd)) {
            return 512;
        }

        unsigned int lbs;
        if (::ioctl(fd, BLKSSZGET, &lbs) < 0) {
            throw LibcError(errno, "Geting logical block size failed: ");
        }
        assert(lbs > 0);
        return lbs;
    }

    static bool isBlockDeviceStatic(int fd) {
        assert(fd >= 0);

        struct stat s;
        statStatic(fd, &s);
        return (s.st_mode & S_IFMT) == S_IFBLK;
    }

    /**
     * Helper function for constructor.
     * Get device size in bytes.
     *
     * RETURN:
     *   device size [bytes].
     * EXCEPTION:
     *   std::runtime_error.
     */
    static uint64_t getDeviceSizeStatic(int fd) {

        if (isBlockDeviceStatic(fd)) {
            uint64_t size;
            if (::ioctl(fd, BLKGETSIZE64, &size) < 0) {
                throw LibcError(errno, "ioctl failed: ");
            }
            return size;
        } else {
            struct stat s;
            statStatic(fd, &s);
            return static_cast<uint64_t>(s.st_size);
        }
    }
};

/**
 * Simple Ring buffer for struct T.
 */
template<typename T>
class DataBuffer
{
private:
    const size_t size_;
    size_t idx_;
    size_t allocated_;
    std::vector<bool> bmp_;
    std::vector<T> data_;

public:
    explicit DataBuffer(size_t size)
        : size_(size)
        , idx_(0)
        , allocated_(0)
        , bmp_(size, false)
        , data_(size) {}

    /**
     * RETURN:
     *   buffer pointer in success, or nullptr.
     */
    T* alloc() {
        if (allocated_ >= size_) {
            return nullptr;
        }
        if (bmp_[idx_]) {
            return nullptr;
        }
        T *p = &data_[idx_];
        //::fprintf(::stderr, "alloc %zu %p\n", idx_, p); //debug
        bmp_[idx_] = true;
        allocated_++;
        idx_ = (idx_ + 1) % size_;
        return p;
    }

    /**
     * Do not call non-allocated buffer.
     */
    void free(T *p) {
        size_t i = toIdx(p);
        //::fprintf(::stderr, "free %zu %p %d\n", i, p, bmp_[i] ? 1 : 0); //debug
        assert(bmp_[i]);
        assert(allocated_ > 0);
        bmp_[i] = false;
        allocated_--;
    }
private:
    size_t toIdx(T *p) const {
        uintptr_t p0 = reinterpret_cast<uintptr_t>(&data_[0]);
        uintptr_t p1 = reinterpret_cast<uintptr_t>(p);
        constexpr size_t s = sizeof(T);
        assert(p0 <= p1);
        assert(p1 < p0 + size_ * s);
        assert((p1 - p0) % s == 0);
        return (p1 - p0) / s;
    }
};

/**
 * Ring buffer for block data.
 * Each block memory is aligned.
 *
 * typename T must be (char) or (unsigned char).
 */
template<typename T>
class BlockBuffer
{
private:
    const size_t nr_;
    const size_t blockSize_;
    std::vector<bool> bmp_;
    T *ary_;
    size_t idx_;
    size_t allocated_;

public:
    BlockBuffer(size_t nr, size_t alignment, size_t blockSize)
        : nr_(nr)
        , blockSize_(blockSize)
        , bmp_(nr, false)
        , ary_(nullptr)
        , idx_(0)
        , allocated_(0) {
        assert(blockSize % alignment == 0);
        int ret = ::posix_memalign((void **)&ary_, alignment, blockSize * nr);
        if (ret) {
            throw std::bad_alloc();
        }
        assert(ary_ != nullptr);
    }

    ~BlockBuffer() {
        ::free(ary_);
    }

    /**
     * RETURN:
     *   Buffer pointer in success, or nullptr.
     */
    T* alloc() {
        if (allocated_ >= nr_) {
            return nullptr;
        }
        if (bmp_[idx_]) {
            return nullptr;
        }
        T *p = &ary_[idx_ * blockSize_];
        //::fprintf(::stderr, "alloc %zu %p\n", idx_, p); //debug
        bmp_[idx_] = true;
        allocated_++;
        idx_ = (idx_ + 1) % nr_;
        return p;
    }

    /**
     * Do not call this non-allocated buffers.
     */
    void free(T *p) {
        size_t i = toIdx(p);
        //::fprintf(::stderr, "free %zu %p\n", i, p); //debug
        assert(bmp_[i]);
        assert(allocated_ > 0);
        allocated_--;
        bmp_[i] = false;
    }

private:
    size_t toIdx(T *p) const {
        uintptr_t pu0 = reinterpret_cast<uintptr_t>(ary_);
        uintptr_t pu1 = reinterpret_cast<uintptr_t>(p);

        assert(pu0 <= pu1);
        assert(pu1 < pu0 + (nr_ * blockSize_));
        assert((pu1 - pu0) % blockSize_ == 0);

        return (pu1 - pu0) / blockSize_;
    }
};

/**
 * Allocator for aligned memory blocks.
 */
template<typename T>
static inline
std::shared_ptr<T> allocateBlock(size_t alignment, size_t size)
{
    T *p = nullptr;
    int ret = ::posix_memalign((void **)&p, alignment, size);
    if (ret) {
        throw std::bad_alloc();
    }
    assert(p != nullptr);
    //::printf("allocated %p\n", p);
    return std::shared_ptr<T>(p, [](T *p) {
            //::printf("freed %p\n", p);
            ::free(p);
        });
}

/**
 * Fast aligned memory block allocator
 * with pre-allocated ring buffer.
 */
template<typename T>
class BlockAllocator
{
private:
    const size_t nr_;
    const size_t alignment_;
    const size_t size_;
    BlockBuffer<T> bb_;

public:
    BlockAllocator(size_t nr, size_t alignment, size_t size)
        : nr_(nr)
        , alignment_(alignment)
        , size_(size)
        , bb_(nr, alignment, size) {}

    /**
     * Allocate block.
     * If allocation failed from ring buffer,
     * this will allocate memory directly.
     */
    std::shared_ptr<T> alloc() {
        T *p = bb_.alloc();
        if (p != nullptr) {
            return std::shared_ptr<T>(p, [&](T *p) {
                    bb_.free(p);
                });
        } else {
            return allocateBlock<T>(alignment_, size_);
        }
    }

    size_t blockSize() const { return size_; }
};

/**
 * Convert size string with unit suffix to unsigned integer.
 */
uint64_t fromUnitIntString(const std::string &valStr)
{
    int shift = 0;
    std::string s(valStr);
    const size_t sz = s.size();

    if (sz == 0) { RT_ERR("Invalid argument."); }
    switch (s[sz - 1]) {
    case 'e':
    case 'E':
        shift += 10;
    case 'p':
    case 'P':
        shift += 10;
    case 't':
    case 'T':
        shift += 10;
    case 'g':
    case 'G':
        shift += 10;
    case 'm':
    case 'M':
        shift += 10;
    case 'k':
    case 'K':
        shift += 10;
        s.resize(sz - 1);
    case '0':
    case '1':
    case '2':
    case '3':
    case '4':
    case '5':
    case '6':
    case '7':
    case '8':
    case '9':
        break;
    default:
        RT_ERR("Invalid suffix charactor.");
    }
    assert(shift < 64);

    for (size_t i = 0; i < sz - 1; i++) {
        if (!('0' <= valStr[i] && valStr[i] <= '9')) {
            RT_ERR("Not numeric charactor.");
        }
    }
    uint64_t val = atoll(s.c_str());
    uint64_t mask = (1ULL << (64 - shift)) - 1;
    if ((val & mask) != val) {
        RT_ERR("fromUnitIntString: overflow.");
    }
    return val << shift;
}

/**
 * Random number generator for UIntType.
 */
template<typename UIntType>
class Rand
{
private:
    std::random_device rd_;
    std::mt19937 gen_;
    std::uniform_int_distribution<UIntType> dist_;
public:
    Rand()
        : rd_()
        , gen_(rd_())
        , dist_(0, UIntType(-1)) {}

    UIntType get() {
        return dist_(gen_);
    }
};

/**
 * Unit suffixes:
 *   k: 2^10
 *   m: 2^20
 *   g: 2^30
 *   t: 2^40
 *   p: 2^50
 *   e: 2^60
 */
std::string toUnitIntString(uint64_t val)
{
    uint64_t mask = (1ULL << 10) - 1;
    char units[] = " kmgtpe";

    size_t i = 0;
    while (i < sizeof(units)) {
        if ((val & mask) != val) { break; }
        i++;
        val >>= 10;
    }

    if (i > 0) {
        return formatString("%" PRIu64 "%c", val, units[i]);
    } else {
        return formatString("%" PRIu64 "", val);
    }
}

void testUnitIntString()
{
    auto check = [](const std::string &s, uint64_t v) {
        CHECKx(fromUnitIntString(s) == v);
        CHECKx(toUnitIntString(v) == s);
    };
    check("12345", 12345);
    check("1k", 1ULL << 10);
    check("2m", 2ULL << 20);
    check("3g", 3ULL << 30);
    check("4t", 4ULL << 40);
    check("5p", 5ULL << 50);
    check("6e", 6ULL << 60);

    /* Overflow check. */
    try {
        fromUnitIntString("7e");
        CHECKx(true);
        fromUnitIntString("8e");
        CHECKx(false);
    } catch (std::runtime_error &e) {
    }
    try {
        fromUnitIntString("16383p");
        CHECKx(true);
        fromUnitIntString("16384p");
        CHECKx(false);
    } catch (std::runtime_error &e) {
    }
}

/**
 * Print byte array as hex list.
 */
template <typename ByteType>
void printByteArray(ByteType *data, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        ::printf("%02x", static_cast<uint8_t>(data[i]));

        if (i % 64 == 63) { ::printf("\n"); }
    }
    if (size % 64 != 0) { ::printf("\n"); }
}

} //namespace util
} //namespace walb

#endif /* UTIL_HPP */
