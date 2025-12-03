#include "qemu/osdep.h"
#include "net/pdes-engine.h"
#include "net/pdes-communicator.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"

struct message_receive_context {
    PDESEngine *engine;
    Message msg;
    QEMUTimer *one_time_poll_timer;
};

uint64_t get_current_virtual_for_normal_message(PDESEngine *engine) {
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + engine->latencyns;
}

uint64_t get_current_virtual_for_destroy_message(PDESEngine *engine) {
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

PDESEngine *pdes_engine_create(
    const char *shm_send, 
    const char *shm_recv, 
    bool sync, 
    uint64_t latencyns,
    PDESRecvCallback cb, 
    void *opaque
) {
    PDESEngine *engine = g_new0(PDESEngine, 1);
    engine->comm = pdes_comm_create(shm_send, shm_recv);
    engine->needs_sync = sync;
    engine->latencyns = latencyns;
    engine->recv_cb = cb;
    engine->recv_opaque = opaque;
    engine->has_first_sync = false;
    engine->waiting_for_quanta = false;
    engine->pair_has_finished = false;
    engine->base_diff = 0;



    engine->msg_rec_poll_timer = timer_new_ns(QEMU_CLOCK_HOST, pdes_engine_poll, engine);
    // Schedule it IMMEDIATELY

    // TODO look into optimizing this
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+5000000); // 5 ms

    uint64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    engine->first_sync_time = current_time;
    
    printf(">>>>>>> NET_INIT_PDES CALLED <<<<<<<\n");
    return engine;
}


void pdes_engine_destroy(PDESEngine *engine) {
    printf("==========================================Destroying PDES Engine...==========================================\n");
    Message mssg = create_message(NULL, 0, END_OF_EMULATION, get_current_virtual_for_destroy_message(engine));
    pdes_comm_send(engine->comm, &mssg);
    if (engine->comm) {
        pdes_comm_destroy(engine->comm);
    }
    g_free(engine);
    printf("==========================================PDES Engine destroyed.==========================================\n");
}

int pdes_engine_send(PDESEngine *engine, const uint8_t *data, size_t len) {
    /* TODO: Add your PDES decision logic here */
    Message mssg = create_message(data, len, MSG_TYPE_NORMAL, get_current_virtual_for_normal_message(engine));
    // Get current virtual time and add latency
    mssg.ts_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + (engine->latencyns);
    // printf("===========================================PDES Engine: Sending message of length %zu with timestamp %lu ns==========================================\n", len, mssg.ts_ns);
    return pdes_comm_send(engine->comm, &mssg);
}



void process_message(PDESEngine *engine, Message *msg) {
    engine->recv_cb(engine->recv_opaque, msg);
}

void pdes_engine_poll(void *opaque) {
    u_int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    PDESEngine *engine = opaque;
    Message msg;

    int len = pdes_comm_recv(engine->comm, &msg);
    
    if (len == NO_MESSAGE) {
        // No message available
        // TODO see if anything needs to happen here
    }
    else if (len < 0) {
        // Error handling
        fprintf(stderr, "Error receiving message: %d\n", len);
    }else{
        process_message(engine, &msg);    
    }

    schedule_poll(engine);
}

void schedule_poll(void *opaque){
    PDESEngine *engine = opaque;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+5000000); // 5 ms
}