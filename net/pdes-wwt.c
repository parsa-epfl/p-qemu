#include "qemu/osdep.h"
#include "net/pdes-communicator.h"
#include "net/pdes-engine.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"



int64_t get_current_virtual_for_sync_message(PDESEngine *engine) {
    return get_universal_virtual_time(engine);
}

PDESWWT *pdes_engine_wwt_create(
    const char *shm_send,
    const char *shm_recv,
    bool sync,
    int64_t latencyns,
    PDESFinalRecvCallback cb, 
    void *opaque
){
    // Create WWT specific engine
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    int64_t time_to_setup = current_time + 1;

    PDESWWT *wwt = g_new0(PDESWWT, 1);

    // TODO make sure this is always done first here and for the engine
    

    // Creating underlying PDESEngine
    wwt->engine = pdes_engine_create(
        shm_send, 
        shm_recv, 
        sync, 
        latencyns, 
        wwt_recivied_callback, 
        wwt,
        is_waiting_for_quanta,
        wwt,
        time_to_setup
    );
    

    // Setup final callback and opaque for when receiving messages
    wwt->recv_opaque = opaque;
    wwt->recv_cb = cb;


    
    wwt->quantum_ns = latencyns;
    wwt->latencyns = latencyns;
    wwt->number_of_neighbors = 1;
    wwt->number_of_neighbors_finished = 0;
    wwt->should_sync = sync;
    wwt->has_finished = false;

    // Setup timer to call setup_wwt
    wwt->setup_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)setup_wwt, wwt);
    // get current ns time and start in 1 ns
    timer_mod(wwt->setup_timer, time_to_setup);


    // TODO implement should sync 
    wwt->quantum_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)quanta_sync, wwt);
    // Schedule for first quantum which is based on latencyns
    timer_mod(wwt->quantum_timer, current_time + wwt->quantum_ns);

    return wwt;
}



void setup_wwt(PDESWWT *wwt_engine){
    // Send initial sync message
    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    send_sync(wwt_engine);

    // assert setup happened at the right time
    // assert(current_time == wwt_engine->first_sync_virtual_time && "WWT setup called at the wrong time");
    // TODO above assertion fails due to how time is managed in qemu , and since other messages are sent out
    // The blow hack is used, so fake first time then a correction to the time here
    // TODO see if this can be fixed later
    wwt_engine->engine->first_sync_virtual_time = current_time;
    // TODO below is caused by the same problem, this marks the time diff as not calculated so it will be recalculated on first message
    wwt_engine->engine->caclulated_time_diff = false;


    printf("WWT: Setup called at virtual time %lu ns (first sync time was %lu ns).\n", current_time, wwt_engine->engine->first_sync_virtual_time);

    printf("WWT: Setup called, sent initial sync message.\n");


    pdes_pause(wwt_engine->engine);

    printf("WWT: All neighbors finished setup.\n");
    // Reset for next quantum
    // TODO address the bug that may be caused without sync (as you can see multiple sync messages at once)
    wwt_engine->number_of_neighbors_finished = 0;
}

void send_sync(PDESWWT *wwt_engine){
    Message sync_msg = create_message(NULL, 0, MSG_TYPE_SYNC, get_current_virtual_for_sync_message(wwt_engine->engine));
    pdes_engine_send(wwt_engine->engine, &sync_msg);
}
void finish_quantum(PDESWWT *wwt_engine){
    send_sync(wwt_engine);
}
int wwt_send(PDESWWT *wwt_engine, const uint8_t *data, size_t len){
    Message msg = create_message(data, len, MSG_TYPE_NORMAL, get_universal_virtual_time(wwt_engine->engine) + (wwt_engine->latencyns)); 
    return pdes_engine_send(wwt_engine->engine, &msg);
}

// TODO put its type to PDESRecvCallback
void wwt_recivied_callback(void *opaque, Message *msg){
    PDESWWT *wwt_engine = (PDESWWT *)opaque;

    int64_t translated_time = msg->ts_ns;
    int64_t current_virtual_time_translated = get_universal_virtual_time(wwt_engine->engine);


    if(msg->type == MSG_TYPE_SYNC){
        // Received sync message from neighbor
        
        // assert that time difference between nodes can not be more than quanta
        // TODO removed due to the host time poll of underlying engine causing issues, needs to be fixed later, should be ok for later syncs still
        // if (abs(translated_time - current_virtual_time) > wwt_engine->quantum_ns) {
        //     printf("WWT Engine received sync message with timestamp %lu ns while current virtual time is %lu ns\n", translated_time, current_virtual_time);
        //     assert(false && "Received sync message with timestamp too far in the future or past");
        // }


        wwt_engine->number_of_neighbors_finished += 1;
    } else if (msg->type == MSG_TYPE_NORMAL){
        // Normal message, pass to final callback
        // Use message timestamp to process it later at correct virtual time
        MessageReceiveContext *ctx = g_new0(MessageReceiveContext, 1);
        ctx->recv_cb = wwt_engine->recv_cb;
        ctx->recv_opaque = wwt_engine->recv_opaque;
        ctx->msg = *msg;
        ctx->one_time_poll_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, (QEMUTimerCB *)process_message_at_virtual_time, ctx);

        int64_t processing_time = translated_time;

        // Process at schedule or now + 1 which ever is later
        if (wwt_engine->should_sync) {
            // If should sync, process exactly at timestamp and throw an error if its in the past
            if (translated_time < current_virtual_time_translated) {
                // Should not happen
                printf("WWT Engine received message with timestamp %lu ns while current virtual time is %lu\n", translated_time, current_virtual_time_translated);
                assert(false && "Received message with timestamp in the past while should_sync is enabled");
            }
        }else{
            processing_time = (translated_time > current_virtual_time_translated + 1) ? translated_time : current_virtual_time_translated + 1;
        }
        timer_mod(ctx->one_time_poll_timer, processing_time);

        wwt_engine->recv_cb(wwt_engine->recv_opaque, msg->data, msg->len);
    }
}


bool is_waiting_for_quanta(PDESWWT *wwt_engine) {
    // TODO this needs to be addressed
    // as this thread can block polling, call message reading after each sleep
    pdes_engine_poll(wwt_engine->engine);
    bool waiting = wwt_engine->number_of_neighbors_finished < wwt_engine->number_of_neighbors;
    waiting = waiting && (!wwt_engine->engine->checkpoint_in_progress);
    if (waiting == false){
        pdes_play(wwt_engine->engine);
    }
    return waiting;
}



void quanta_sync(PDESWWT *wwt_engine){
    // Sends sync, pauses and waits for others sync, then resumes
    // printf("WWT: Starting quantum sync at universal virtual time %lu ns.\n", get_universal_virtual_time(wwt_engine->engine));
    send_sync(wwt_engine);

    // same using is_waiting_for_quanta as setup, as its the same logic
    pdes_pause(wwt_engine->engine);

    // TODO this has a race condition of edge (if checkpoint changes after pause), need to address
    // if we came back due to checkpoint in progress
    if (wwt_engine->engine->checkpoint_in_progress){
        // schedule for next quantum after checkpoint is done, so that it's recalled immediately
        int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        // TODO check if this causes bugs for qemu, if it does, come up with system for both nodes to go forward to an agreed timestamp
        timer_mod(wwt_engine->quantum_timer, current_time + 1);
    }else{
        // Schedule next quantum
        int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        timer_mod(wwt_engine->quantum_timer, current_time + wwt_engine->quantum_ns);
        // printf("WWT: Quantum sync completed at universal virtual time %lu ns.\n", get_universal_virtual_time(wwt_engine->engine));

        // Reset for next quantum
        wwt_engine->number_of_neighbors_finished = 0;
    }
    
}