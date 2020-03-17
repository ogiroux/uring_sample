#include <atomic>
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

/*
    My intention is to use SQPOLL, but right now I'm avoiding it.

    p.flags |= IORING_SETUP_SQPOLL;
    p.sq_thread_idle = 1000000; //< Long time.
*/

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
        unsigned *array;
    } request = {
        (unsigned *)(sqptr + p.sq_off.head),
        (unsigned *)(sqptr + p.sq_off.tail),
        (unsigned *)(sqptr + p.sq_off.ring_mask),
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

    int infd = open("myfile", O_RDONLY);
    assert(infd > 0);

    uint32_t sum = 0;
    uint32_t buff[1<<18];
    iovec vec = { buff, 1<<20 };

    std::memset(sqentriesptr+0, 0, sizeof(io_uring_sqe));
    sqentriesptr[0].fd = infd;
    sqentriesptr[0].opcode = IORING_OP_READV;
    sqentriesptr[0].addr = (uint64_t)&vec;
    sqentriesptr[0].len = 1;
    for(int i = 0; i < (1<<30); ++i) 
    {
        sqentriesptr[0].off = i<<20;

        auto const off = reinterpret_cast<std::atomic_int&>(*request.tail).load(std::memory_order_relaxed) & *request.ring_mask;
        request.array[off] = 0;
        reinterpret_cast<std::atomic_int&>(*request.tail) = (off + 1) & *request.ring_mask;

        auto const headptr = *response.head & *response.ring_mask;
        while(1)
        {
            auto const tailptr = reinterpret_cast<std::atomic_int&>(*response.tail).load(std::memory_order_relaxed) & *response.ring_mask;
            if(headptr != tailptr)
                break;
            // Just to prod it along
            auto const enter = __sys_io_uring_enter(ioring_fd, 1, 1, 0, NULL);
            if(enter < 0) {
                std::cout << "Error in enter : " << errno << std::endl;
                abort();
            }
        }
        auto const res = reinterpret_cast<std::atomic_int&>(response.array[headptr].res).load(std::memory_order_relaxed);
        if(res < 0) {
            std::cout << res << std::endl;
            abort();
        }
        reinterpret_cast<std::atomic_int&>(*response.head) = (headptr + 1) & *response.ring_mask;

        sum = std::accumulate(buff, buff + (1<<18), sum);
        std::cout << "\rSum: " << sum;
    }
    std::cout << "\n";

    close(infd);
    close(ioring_fd);

    return 0;
}