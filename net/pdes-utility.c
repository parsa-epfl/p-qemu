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


int64_t get_transformed_timestamp(PDESEngine *engine, Message *msg) {
    // Find the difference between first sync times
    if(engine->first_sync_virtual_time == -1 || engine->neighbor_first_sync_virtual_time == -1){
        // This should not happen, as the first message sent out should be syncs
        assert(false && "First sync virtual times not set before transforming timestamp");
    }
    
    if (engine->caclulated_time_diff == false){
        // First time calculating base time diff
        engine->base_time_diff = engine->first_sync_virtual_time - engine->neighbor_first_sync_virtual_time;
        printf("PDES Engine calculated base time difference of %lu ns (first sync virtual time: %lu ns, neighbor first sync virtual time: %lu ns)\n", engine->base_time_diff, engine->first_sync_virtual_time, engine->neighbor_first_sync_virtual_time);
        engine->caclulated_time_diff = true;
    }
    // Transform the message timestamp
    return msg->ts_ns + engine->base_time_diff;
}