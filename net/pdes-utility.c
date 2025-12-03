#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "net/pdes-engine.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"

void process_message_at_virtual_time(MessageReceiveContext *opaque) {
    struct MessageReceiveContext *ctx = opaque;
    Message *msg = &ctx->msg;


    if (msg->len > 0 && msg->type != MSG_TYPE_SYNC && ctx->recv_cb) {
        // Assert the time has arrived
        int64_t current_virtual_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        
        printf("****sending message of length %u to recv callback at time %lu ns****\n", msg->len, current_virtual_time);
        ctx->recv_cb(ctx->recv_opaque, msg->data, msg->len);
    }else{
        // Should not get here
        assert(false && "process_message_at_virtual_time called with invalid message or no recv_cb");
    }

    g_free(ctx->one_time_poll_timer);
    g_free(ctx);
}
