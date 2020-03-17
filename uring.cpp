#include <atomic>
#include <chrono>
#include <thread>
#include <iostream>
#include <numeric>
#include <cstring>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <liburing.h>
#include "syscall.h"

static const int ioring_size = 16;

int main()
{
    io_uring_params p;
    std::memset(&p, 0, sizeof(p));

    p.flags |= IORING_SETUP_SQPOLL;
    p.sq_thread_idle = 1000000; //< Long time.

    int ioring_fd = __sys_io_uring_setup(ioring_size, &p);
    if(ioring_fd < 0) {
        std::cout << "Error in setup : " << errno << std::endl;
        abort();
    }

    auto const sqptr = (__u8*)mmap(0, p.sq_off.array + p.sq_entries * sizeof(__u32),
                                PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE,
                                ioring_fd, IORING_OFF_SQ_RING);
    assert(sqptr != MAP_FAILED);
    struct app_sq_ring {
        unsigned *head;
        unsigned *tail;
        unsigned *ring_mask;
        unsigned *flags;
        unsigned *array;
    } request = {
        (unsigned *)(sqptr + p.sq_off.head),
        (unsigned *)(sqptr + p.sq_off.tail),
        (unsigned *)(sqptr + p.sq_off.ring_mask),
        (unsigned *)(sqptr + p.sq_off.flags),
        (unsigned *)(sqptr + p.sq_off.array)
    };

    auto const sqentriesptr = (io_uring_sqe*)mmap(0, p.sq_entries * sizeof(io_uring_sqe),
                                PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE,
                                ioring_fd, IORING_OFF_SQES);
    assert(sqentriesptr != MAP_FAILED);

    auto const cqptr = (__u8*)mmap(0, p.cq_off.cqes + p.cq_entries * sizeof(io_uring_cqe),
                        PROT_READ|PROT_WRITE, MAP_SHARED|MAP_POPULATE, ioring_fd,
                        IORING_OFF_CQ_RING);
    assert(cqptr != MAP_FAILED);

    struct app_cq_ring {
        unsigned *head;
        unsigned *tail;
        unsigned *ring_mask;
        io_uring_cqe *array;
    } response = {
        (unsigned *)(cqptr + p.cq_off.head),
        (unsigned *)(cqptr + p.cq_off.tail),
        (unsigned *)(cqptr + p.cq_off.ring_mask),
        (io_uring_cqe *)(cqptr + p.cq_off.cqes)
    };

    int _infd = open("myfile", O_RDONLY);
    assert(_infd > 0);

    int infd = __sys_io_uring_register(ioring_fd,  IORING_REGISTER_FILES, &_infd, 1);
    assert(_infd > 0);

    uint32_t sum = 0;
    uint32_t buff[1<<18];
    iovec vec = { buff, 1<<20 };

    std::atomic_bool done = {false};
    std::thread helper([&](){
        while(!done) {
            if (reinterpret_cast<std::atomic_uint&>(*request.flags) & IORING_SQ_NEED_WAKEUP)
                __sys_io_uring_enter(ioring_fd, 1, 0, IORING_ENTER_SQ_WAKEUP, NULL);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    for(int i = 0; i < (1<<10); ++i)
    {
        // request
        {
            std::memset(sqentriesptr+0, 0, sizeof(io_uring_sqe));
            sqentriesptr[0].fd = infd;
            sqentriesptr[0].opcode = IORING_OP_READV;
            sqentriesptr[0].addr = (uint64_t)&vec;
            sqentriesptr[0].len = 1;
            sqentriesptr[0].off = i<<20;
            sqentriesptr[0].flags = IOSQE_FIXED_FILE;

            unsigned const tail = reinterpret_cast<std::atomic_uint&>(*request.tail);
            while(1)
            {
                unsigned const head = reinterpret_cast<std::atomic_uint&>(*request.head);
                if(head != ((tail + 1) & *request.ring_mask))
                    break;
            }
            request.array[tail & *request.ring_mask] = 0;
            reinterpret_cast<std::atomic_uint&>(*request.tail) = (tail + 1);
        }
        // response
        {
            unsigned const head = reinterpret_cast<std::atomic_uint&>(*response.head);
            while(1)
            {
                unsigned const tail = reinterpret_cast<std::atomic_uint&>(*response.tail);
                if(head != tail)
                    break;
            }
            if(response.array[head & *response.ring_mask].res < 0) {
                std::cout << response.array[head & *response.ring_mask].res << std::endl;
                abort();
            }
            reinterpret_cast<std::atomic_uint&>(*response.head) = (head + 1);
            sum = std::accumulate(buff, buff + (1<<18), sum);
        }
    }
    std::cout << " sum: " << std::dec << sum << std::endl;

    done = true;
    helper.join();
    __sys_io_uring_register(ioring_fd,  IORING_UNREGISTER_FILES, &_infd, 1);
    close(infd);
    close(ioring_fd);

    return 0;
}
