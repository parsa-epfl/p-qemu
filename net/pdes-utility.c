#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "net/pdes-engine.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "net/pdes-checkpoint.h"

void process_message_at_virtual_time(MessageReceiveContext *opaque) {
    struct MessageReceiveContext *ctx = opaque;
    Message *msg = &ctx->msg;


    if (msg->len > 0 && msg->type != MSG_TYPE_SYNC && ctx->recv_cb) {
        // Assert the time has arrived
        ctx->recv_cb(ctx->recv_opaque, msg->data, msg->len);
    }else{
        // Should not get here
        assert(false && "process_message_at_virtual_time called with invalid message or no recv_cb");
    }

    pdes_inflight_remove(&ctx->msg, ctx->timestamp_ns);
    g_free(ctx->one_time_poll_timer);
    g_free(ctx);
}


int64_t get_universal_virtual_time(PDESEngine *engine) {
    // Find the difference between first sync times
    if(engine->first_sync_virtual_time == -1){
        // This should not happen, as the first message sent out should be syncs
        assert(false && "First sync virtual times not set before transforming timestamp");
    }

    // get current time
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    
    // Transform the message timestamp
    return current_time - engine->first_sync_virtual_time;
}