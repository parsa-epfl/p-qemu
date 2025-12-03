#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "net/pdes-engine.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"



uint64_t get_current_virtual_for_sync_message(PDESEngine *engine) {
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

PDESWWT *pdes_engine_wwt_create(
    const char *shm_send,
    const char *shm_recv,
    bool sync,
    uint64_t latencyns,
    PDESFinalRecvCallback cb, 
    void *opaque
){
    // Create WWT specific engine
    PDESWWT *wwt = g_new0(PDESWWT, 1);


    // Creating underlying PDESEngine
    wwt->engine = pdes_engine_create(
        shm_send, 
        shm_recv, 
        sync, 
        latencyns, 
        wwt_recivied_callback, 
        wwt,
        is_waiting_for_quanta,
        wwt
    );
    

    // Setup final callback and opaque for when receiving messages
    wwt->recv_opaque = opaque;
    wwt->recv_cb = cb;


    
    wwt->quantum_ns = latencyns;
    wwt->number_of_neighbors = 1;
    wwt->number_of_neighbors_finished = 0;
    wwt->should_sync = sync;
    wwt->has_finished = false;

    // Setup timer to call setup_wwt
    wwt->setup_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)setup_wwt, wwt);
    // get current ns time and start in 1 ns
    uint64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    timer_mod(wwt->setup_timer, current_time + 1);


    // TODO implement should sync 
    wwt->quantum_timer = NULL; 
    return wwt;
}



void setup_wwt(PDESWWT *wwt_engine){
    // Send initial sync message
    send_sync(wwt_engine);
    printf("WWT: Setup called, sent initial sync message.\n");


    pdes_pause(wwt_engine->engine);

    printf("WWT: All neighbors finished setup.\n");
    // Reset for next quantum
    // TODO address the bug that may be caused without sync (as you can see multiple sync messages at once)
    wwt_engine->number_of_neighbors_finished = 0;
}

void send_sync(PDESWWT *wwt_engine){
    Message sync_msg = create_message(NULL, 0, MSG_TYPE_SYNC, get_current_virtual_for_sync_message(wwt_engine->engine));
    pdes_comm_send(wwt_engine->engine->comm, &sync_msg);
}
void finish_quantum(PDESWWT *wwt_engine){
    send_sync(wwt_engine);
}
int wwt_send(PDESWWT *wwt_engine, const uint8_t *data, size_t len){
    return pdes_engine_send(wwt_engine->engine, data, len);
}

// TODO put its type to PDESRecvCallback
void wwt_recivied_callback(void *opaque, Message *msg){
    PDESWWT *wwt_engine = (PDESWWT *)opaque;

    if(msg->type == MSG_TYPE_SYNC){
        // Received sync message from neighbor
        wwt_engine->number_of_neighbors_finished += 1;
        printf("WWT: Received sync message. Total finished neighbors: %lu/%lu\n", wwt_engine->number_of_neighbors_finished, wwt_engine->number_of_neighbors);
    } else {
        // Normal message, pass to final callback
        // Use message timestamp to process it later at correct virtual time
        MessageReceiveContext *ctx = g_new0(MessageReceiveContext, 1);
        ctx->recv_cb = wwt_engine->recv_cb;
        ctx->recv_opaque = wwt_engine->recv_opaque;
        ctx->msg = *msg;
        ctx->one_time_poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)process_message_at_virtual_time, ctx);

        uint64_t current_virtual_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        // Process at schedule or now + 1 which ever is later
        uint64_t process_time = (ctx->msg.ts_ns > current_virtual_time + 1) ? ctx->msg.ts_ns : current_virtual_time + 1;
        timer_mod(ctx->one_time_poll_timer, process_time);

        wwt_engine->recv_cb(wwt_engine->recv_opaque, msg->data, msg->len);
    }
}


bool is_waiting_for_quanta(PDESWWT *wwt_engine) {
    // TODO this needs to be addressed
    // as this thread can block polling, call message reading after each sleep
    pdes_engine_poll(wwt_engine->engine);
    bool waiting = wwt_engine->number_of_neighbors_finished < wwt_engine->number_of_neighbors;
    if (waiting == false){
        pdes_play(wwt_engine->engine);
    }
    return waiting;
}