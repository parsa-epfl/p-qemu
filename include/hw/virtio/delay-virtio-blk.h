/*
 * Delayed-Response Virtio Block Device
 *
 * A variation of virtio-blk whose request completions are gated by a
 * virtual-time timer so that the guest observes a configurable, constant
 * end-to-end I/O latency.  See docs/devel/delay-virtio-blk.rst for the full
 * design rationale.
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#ifndef QEMU_DELAY_VIRTIO_BLK_H
#define QEMU_DELAY_VIRTIO_BLK_H

#include "standard-headers/linux/virtio_blk.h"
#include "hw/virtio/virtio.h"
#include "hw/block/block.h"
#include "sysemu/block-backend.h"
#include "sysemu/block-ram-registrar.h"
#include "qemu/queue.h"
#include "qemu/timer.h"
#include "qom/object.h"
/* Reuse VirtIOBlkConf, struct virtio_blk_inhdr and related definitions. */
#include "hw/virtio/virtio-blk.h"

#define TYPE_DELAY_VIRTIO_BLK "delay-virtio-blk"
OBJECT_DECLARE_SIMPLE_TYPE(DelayVirtIOBlock, DELAY_VIRTIO_BLK)

struct DelayVirtIOBlockReq;

struct DelayVirtIOBlock {
    VirtIODevice parent_obj;
    BlockBackend *blk;
    struct DelayVirtIOBlockReq *rq;  /* error queue (BLOCK_ERROR_ACTION_STOP) */
    VirtIOBlkConf conf;              /* reused from virtio-blk.h */
    unsigned short sector_mask;
    bool original_wce;
    VMChangeStateEntry *change;
    uint64_t host_features;
    size_t config_size;
    BlockRAMRegistrar blk_ram_registrar;

    /* Delay-specific state */
    QEMUTimer *delay_timer;          /* one-shot, armed to earliest deadline */
    QTAILQ_HEAD(, DelayVirtIOBlockReq) delay_queue;  /* sorted by deadline_ns */

    /* Configurable properties (microseconds) */
    uint64_t short_delay_us;
    uint64_t long_delay_us;
    uint64_t merge_overhead_us;
};

typedef struct DelayVirtIOBlockReq {
    /* Layout mirrors VirtIOBlockReq so the existing request logic applies. */
    VirtQueueElement elem;
    int64_t sector_num;
    DelayVirtIOBlock *dev;
    VirtQueue *vq;
    IOVDiscardUndo inhdr_undo;
    IOVDiscardUndo outhdr_undo;
    struct virtio_blk_inhdr *in;
    struct virtio_blk_outhdr out;
    QEMUIOVector qiov;
    size_t in_len;
    struct DelayVirtIOBlockReq *next;     /* error queue */
    struct DelayVirtIOBlockReq *mr_next;  /* merge chain */
    BlockAcctCookie acct;

    /* Delay-specific fields */
    int64_t deadline_ns;                  /* absolute completion deadline */
    bool io_done;                         /* real I/O has finished */
    uint8_t io_status;                    /* saved completion status */
    QTAILQ_ENTRY(DelayVirtIOBlockReq) queue_link;  /* delay queue entry */
} DelayVirtIOBlockReq;

#define DELAY_VIRTIO_BLK_MAX_MERGE_REQS 32

typedef struct DelayMultiReqBuffer {
    DelayVirtIOBlockReq *reqs[DELAY_VIRTIO_BLK_MAX_MERGE_REQS];
    unsigned int num_reqs;
    bool is_write;
} DelayMultiReqBuffer;

void delay_virtio_blk_handle_vq(DelayVirtIOBlock *s, VirtQueue *vq);

#endif /* QEMU_DELAY_VIRTIO_BLK_H */
