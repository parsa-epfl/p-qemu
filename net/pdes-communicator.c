#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "qemu/atomic.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MAX_MSG_SIZE 2048

typedef struct {
    uint32_t len;
    uint8_t data[MAX_MSG_SIZE];
} Message;

typedef struct {
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
    Message messages[16];
} ShmRing;

struct PDESCommunicator {
    int fd;
    ShmRing *ring;
    size_t size;
};

PDESCommunicator *pdes_comm_create(const char *shm_name, size_t shm_size) {
    PDESCommunicator *comm = g_new0(PDESCommunicator, 1);
    
    comm->fd = shm_open(shm_name, O_CREAT | O_RDWR, 0666);
    if (comm->fd < 0) {
        g_free(comm);
        return NULL;
    }
    
    if (ftruncate(comm->fd, shm_size) < 0) {
        close(comm->fd);
        g_free(comm);
        return NULL;
    }
    
    comm->ring = mmap(NULL, shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, comm->fd, 0);
    comm->size = shm_size;
    
    return comm;
}

void pdes_comm_destroy(PDESCommunicator *comm) {
    if (comm->ring) {
        munmap(comm->ring, comm->size);
    }
    if (comm->fd >= 0) {
        close(comm->fd);
    }
    g_free(comm);
}

int pdes_comm_send(PDESCommunicator *comm, const uint8_t *data, size_t len) {
    if (len > MAX_MSG_SIZE) return -1;
    
    uint32_t next_write = (comm->ring->write_idx + 1) % 16;
    if (next_write == comm->ring->read_idx) return -EAGAIN;
    
    Message *msg = &comm->ring->messages[comm->ring->write_idx];
    msg->len = len;
    memcpy(msg->data, data, len);
    qatomic_set_mb(&comm->ring->write_idx, next_write);
    
    return len;
}

int pdes_comm_recv(PDESCommunicator *comm, uint8_t *buf, size_t buf_len) {
    if (comm->ring->read_idx == comm->ring->write_idx) return 0;
    
    Message *msg = &comm->ring->messages[comm->ring->read_idx];
    size_t len = msg->len < buf_len ? msg->len : buf_len;
    memcpy(buf, msg->data, len);
    qatomic_set_mb(&comm->ring->read_idx, (comm->ring->read_idx + 1) % 16);
    
    return len;
}