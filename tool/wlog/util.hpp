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

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/fs.h>


#define RT_ERR(fmt, args...)                                    \
    std::runtime_error(walb::util::formatString(fmt, ##args))

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

#if 0
/**
 * Each IO log.
 */
struct IoLog
{
    const unsigned int threadId;
    const IoType type;
    const size_t blockId;
    const double startTime; /* unix time [second] */
    const double response; /* [second] */

    IoLog(unsigned int threadId_, IoType type_, size_t blockId_,
          double startTime_, double response_)
        : threadId(threadId_)
        , type(type_)
        , blockId(blockId_)
        , startTime(startTime_)
        , response(response_) {}

    IoLog(const IoLog& log)
        : threadId(log.threadId)
        , type(log.type)
        , blockId(log.blockId)
        , startTime(log.startTime)
        , response(log.response) {}

    void print() {
        ::printf("threadId %d type %d blockId %10zu startTime %.06f response %.06f\n",
                 threadId, (int)type, blockId, startTime, response);
    }
};
#endif

static inline double getTime()
{
    struct timeval tv;
    double t;

    ::gettimeofday(&tv, NULL);

    t = static_cast<double>(tv.tv_sec) +
        static_cast<double>(tv.tv_usec) / 1000000.0;
    return t;
}

class EofError : public std::exception {
    virtual const char *what() const noexcept {
        return "eof error";
    }
};

class FdReader
{
private:
    int fd_;
public:
    FdReader(int fd)
        : fd_(fd) {}

    /**
     * read.
     */
    void read(char *buf, size_t size) {
        size_t s = 0;
        while (s < size) {
            ssize_t ret = ::read(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw RT_ERR("read failed: %s.", ::strerror(errno));
            }
            if (ret == 0) {
                throw EofError();
            }
            s += ret;
        }
    }
};

class FdWriter
{
private:
    int fd_;
public:
    FdWriter(int fd)
        : fd_(fd) {}

    /**
     * write.
     */
    void write(char *buf, size_t size) {
        size_t s = 0;
        while (s < size) {
            ssize_t ret = ::write(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw RT_ERR("write failed: %s.", ::strerror(errno));
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
            throw RT_ERR("fdsync failed: %s.", ::strerror(errno));
        }
    }

    /**
     * fsync.
     */
    void fsync() {
        int ret = ::fsync(fd_);
        if (ret) {
            throw RT_ERR("fsync failed: %s.", ::strerror(errno));
        }
    }
};

class BlockDevice
{
private:
    std::string name_;
    int openFlags_;
    int fd_;
    bool isBlockDevice_;
    size_t deviceSize_; // [bytes].
    unsigned int lbs_; // logical block size [bytes].
    unsigned int pbs_; // physical block size [bytes].

    std::once_flag close_flag_;

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

    ~BlockDevice() { close(); }

    void close() {
        //::fprintf(::stderr, "close called.\n");

        std::call_once(close_flag_, [&]() {
                if (fd_ > 0) {
                    if (::close(fd_) < 0) {
                        ::fprintf(::stderr, "close() failed.\n");
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
            ::fprintf(::stderr, "read %d %p &buf[%zu], %zu\n", fd_, &buf[s], s, size - s);
            ssize_t ret = ::read(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw RT_ERR("read failed: %s.", ::strerror(errno));
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
    void write(off_t oft, size_t size, char* buf) {

        if (deviceSize_ < oft + size) { throw EofError(); }
        ::lseek(fd_, oft, SEEK_SET);
        size_t s = 0;
        while (s < size) {
            ssize_t ret = ::write(fd_, &buf[s], size - s);
            if (ret < 0) {
                throw RT_ERR("write failed: %s.", ::strerror(errno));
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
            throw RT_ERR("fdsync failed: %s.", ::strerror(errno));
        }
    }

    /**
     * fsync.
     */
    void fsync() {

        int ret = ::fsync(fd_);
        if (ret) {
            throw RT_ERR("fsync failed: %s.", ::strerror(errno));
        }
    }

    /**
     * Get device size [byte].
     */
    size_t getDeviceSize() const { return deviceSize_; }

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
            throw RT_ERR("open %s failed: %s.",
                         name.c_str(), ::strerror(errno));
        }
        return fd;
    }

    static void statStatic(int fd, struct stat *s) {

        assert(fd >= 0);
        assert(s);
        if (::fstat(fd, s) < 0) {
            throw RT_ERR("stat failed: %s.", ::strerror(errno));
        }
    }

    static unsigned int getPhysicalBlockSizeStatic(int fd) {

        assert(fd >= 0);

        if (!isBlockDeviceStatic(fd)) {
            return 512;
        }

        unsigned int pbs;
        if (::ioctl(fd, BLKPBSZGET, &pbs) < 0) {
            throw RT_ERR("Getting physical block size failed.");
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
            throw RT_ERR("Geting logical block size failed.");
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
    static size_t getDeviceSizeStatic(int fd) {

        if (isBlockDeviceStatic(fd)) {
            size_t size;
            if (::ioctl(fd, BLKGETSIZE64, &size) < 0) {
                throw RT_ERR("ioctl failed: %s.", ::strerror(errno));
            }
            return size;
        } else {
            struct stat s;
            statStatic(fd, &s);
            return static_cast<size_t>(s.st_size);
        }
    }
};

/**
 * Calculate access range.
 */
static inline size_t calcAccessRange(
    size_t accessRange, size_t blockSize, const BlockDevice& dev) {

    return (accessRange == 0) ? (dev.getDeviceSize() / blockSize) : accessRange;
}


class PerformanceStatistics
{
private:
    double total_;
    double max_;
    double min_;
    size_t count_;

public:
    PerformanceStatistics()
        : total_(0), max_(-1.0), min_(-1.0), count_(0) {}
    PerformanceStatistics(double total, double max, double min, size_t count)
        : total_(total), max_(max), min_(min), count_(count) {}

    void updateRt(double rt) {

        if (max_ < 0 || min_ < 0) {
            max_ = rt; min_ = rt;
        } else if (max_ < rt) {
            max_ = rt;
        } else if (min_ > rt) {
            min_ = rt;
        }
        total_ += rt;
        count_++;
    }

    double getMax() const { return max_; }
    double getMin() const { return min_; }
    double getTotal() const { return total_; }
    size_t getCount() const { return count_; }

    double getAverage() const { return total_ / (double)count_; }

    void print() const {
        ::printf("total %.06f count %zu avg %.06f max %.06f min %.06f\n",
                 getTotal(), getCount(), getAverage(),
                 getMax(), getMin());
    }
};

template<typename T> //T is iterator type of PerformanceStatistics.
static inline PerformanceStatistics mergeStats(const T begin, const T end)
{
    double total = 0;
    double max = -1.0;
    double min = -1.0;
    size_t count = 0;

    std::for_each(begin, end, [&](PerformanceStatistics& stat) {

            total += stat.getTotal();
            if (max < 0 || max < stat.getMax()) { max = stat.getMax(); }
            if (min < 0 || min > stat.getMin()) { min = stat.getMin(); }
            count += stat.getCount();
        });

    return PerformanceStatistics(total, max, min, count);
}

/**
 * Convert throughput data to string.
 */
static inline
std::string getDataThroughputString(double throughput)
{
    const double GIGA = static_cast<double>(1000ULL * 1000ULL * 1000ULL);
    const double MEGA = static_cast<double>(1000ULL * 1000ULL);
    const double KILO = static_cast<double>(1000ULL);

    std::stringstream ss;
    if (throughput > GIGA) {
        throughput /= GIGA;
        ss << throughput << " GB/sec";
    } else if (throughput > MEGA) {
        throughput /= MEGA;
        ss << throughput << " MB/sec";
    } else if (throughput > KILO) {
        throughput /= KILO;
        ss << throughput << " KB/sec";
    } else {
        ss << throughput << " B/sec";
    }

    return ss.str();
}

/**
 * Print throughput data.
 * @blockSize block size [bytes].
 * @nio Number of IO executed.
 * @periodInSec Elapsed time [second].
 */
static inline
void printThroughput(size_t blockSize, size_t nio, double periodInSec)
{
    double throughput = static_cast<double>(blockSize * nio) / periodInSec;
    double iops = static_cast<double>(nio) / periodInSec;
    ::printf("Throughput: %.3f B/s %s %.3f iops.\n",
             throughput, getDataThroughputString(throughput).c_str(), iops);
}

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
    DataBuffer(size_t size)
        : size_(size)
        , idx_(0)
        , allocated_(0)
        , bmp_(size, false)
        , data_(size) {}

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
 */
template<typename T>
class BlockBuffer
{
private:
    const size_t nr_;
    std::vector<bool> bmp_;
    std::unordered_map<uintptr_t, size_t> map_; /* pointer, index. */
    std::vector<T*> ary_;
    size_t idx_;
    size_t allocated_;

public:
    BlockBuffer(size_t nr, size_t alignment, size_t blockSize)
        : nr_(nr)
        , bmp_(nr, false)
        , map_()
        , ary_(nr)
        , idx_(0)
        , allocated_(0) {
        assert(blockSize % alignment == 0);
        for (size_t i = 0; i < nr; i++) {
            T *p = nullptr;
            int ret = ::posix_memalign((void **)&p, alignment, blockSize);
            assert(ret == 0);
            assert(p != nullptr);
            ary_[i] = p;
            map_[reinterpret_cast<uintptr_t>(p)] = i;
        }
    }

    ~BlockBuffer() {
        for (size_t i = 0; i < nr_; i++) {
            ::free(ary_[i]);
        }
    }

    T* alloc() {
        if (allocated_ >= nr_) {
            return nullptr;
        }
        if (bmp_[idx_]) {
            return nullptr;
        }
        T *p = ary_[idx_];
        //::fprintf(::stderr, "alloc %zu %p\n", idx_, p); //debug
        bmp_[idx_] = true;
        allocated_++;
        idx_ = (idx_ + 1) % nr_;
        return p;
    }

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
        uintptr_t pu = reinterpret_cast<uintptr_t>(p);
        assert(map_.find(pu) != map_.end());
        return map_.at(pu);
    }
};

/**
 * Aligned
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

#if 0
template<typename T>
class BlockAllocator
{
private:
    const size_t alignment_;
    const size_t size_;

public:
    BlockAllocator(size_t alignment, size_t size)
        : alignment_(alignment)
        , size_(size) {}

    std::shared_ptr<T> alloc() {
        return allocateBlock<T>(alignment_, size_);
    }
};
#else
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
};
#endif

} //namespace util
} //namespace walb

#endif /* UTIL_HPP */
