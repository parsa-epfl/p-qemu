#include "qemu/osdep.h"
#include "net/pdes-engine.h"
#include "net/pdes-communicator.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "include/migration/snapshot.h"
#include "include/sysemu/runstate.h"
#include "net/pdes-checkpoint.h"

// TODO this should be generlized to multiple neighbours later
// For now singleton pdes engine
extern PDESEngine *singleton_engine = NULL;

PDESEngine *get_singleton_engine(){
    return singleton_engine;
}


int64_t get_current_virtual_for_normal_message(PDESEngine *engine) {
    return get_universal_virtual_time(engine) + engine->latencyns;
}

int64_t get_current_virtual_for_destroy_message(PDESEngine *engine) {
    return get_universal_virtual_time(engine);
}

PDESEngine *pdes_engine_create(
    const char *shm_send, 
    const char *shm_recv, 
    bool sync, 
    int64_t latencyns,
    PDESRecvCallback cb, 
    void *opaque,
    PauseStatusCallBack pause_status_cb,
    void *pause_status_opaque,
    int64_t first_sync_virtual_time,
    bool master
) {
    // Show error if singleton was created before
    assert(singleton_engine == NULL && "Singleton engine already created");
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
    engine->paused = false;
    engine->pause_status_cb = pause_status_cb;
    engine->pause_status_opaque = pause_status_opaque;

    engine->first_sync_virtual_time = first_sync_virtual_time;
    engine->caclulated_time_diff = false;
    engine->base_time_diff = 0;
    engine->neighbour_drained = false;
    engine->checkpoint_in_progress = false;

    engine->master = master;




    engine->msg_rec_poll_timer = timer_new_ns(QEMU_CLOCK_HOST, pdes_engine_poll, engine);
    // Schedule it IMMEDIATELY

    // TODO look into optimizing this
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+50000); // 5 microseconds

    int64_t current_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    // TODO remove this field
    engine->first_sync_time = current_time;
    
    singleton_engine = engine;
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

int pdes_engine_send(PDESEngine *engine, Message *msg) {
    /* TODO: Add your PDES decision logic here */
    if (engine->first_sync_time == -1){
        if (msg->type == MSG_TYPE_SYNC){
            // TODO remove this field
            engine->first_sync_time = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        } else {
            // Should not happen, as first message should be sync
            assert(false && "First message sent is not a sync message");
        }
    }
    return pdes_comm_send(engine->comm, msg);
}


void initiate_checkpoint(void * opaque){
    // Just a temp function to test savevm during drain
    Error *err = NULL;
    printf("PDES Engine performing systemic snapshot save during drain...\n");

    // Get snapshot name from msg data
    Message * msg = (Message *)opaque;
    char snapshot_name[1000];
    memcpy(snapshot_name, msg->data, msg->len);

    save_snapshot(snapshot_name,
                true, NULL, false, NULL, SNAPSHOT_FORMAT_EXTERNAL_ZSTD, &err);
    printf("PDES Engine completed systemic snapshot save during drain.\n");
    if (err) {
        error_reportf_err(err, "Error during temp snapshot save: ");
        exit(1);
    }
}

void process_message(PDESEngine *engine, Message *msg) {
    
    // TODO both drain start and and end are based on just one neighbor for now, need to generalize later
    printf("PDES Engine processing received message of type %u\n", msg->type);
    if (msg->type==DRAIN_START){
        printf("PDES Engine received drain end message, marking drained as true.\n");
        engine->neighbour_drained = true;
        engine->checkpoint_in_progress = true;
        // Check if we are still running and not saving
        printf("got request to initiate checkpoint during drain with snapshot name: %s\n", msg->data);
        if (runstate_check(RUN_STATE_RUNNING)){
            printf("PDES Engine initiating systemic snapshot save after drain.\n");

            // TODO change this so the message includes snapshot name
            // TODO : Ugly solution for now to avoid deadlock:  create a host time timer, call this later, call it immidiately after this
            // TODO We will get stuck thanks to quanta, need to generalize later
            engine->checkpoint_initiate_timer = timer_new_ns(QEMU_CLOCK_REALTIME, initiate_checkpoint, msg);
            timer_mod(engine->checkpoint_initiate_timer, qemu_clock_get_ns(QEMU_CLOCK_REALTIME));
        }
    }else if (msg->type==DRAIN_END){
        printf("PDES Engine received drain end message, marking checkpoint as completed.\n");
        engine->checkpoint_in_progress = false;

    }else{
        // Any other message that comes in, means that we need to get another drain signal
        engine->neighbour_drained = false;
    }

    engine->recv_cb(engine->recv_opaque, msg);
}

void pdes_engine_poll(void *opaque) {
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
    // TODO add a flag so that when calling this manually we don't reschedule again and again
    schedule_poll(engine);
}

void schedule_poll(void *opaque){
    PDESEngine *engine = opaque;
    timer_mod(engine->msg_rec_poll_timer, qemu_clock_get_ns(QEMU_CLOCK_HOST)+50000); // 5 microseconds
}

void pdes_pause(void *opaque){
    PDESEngine *engine = opaque;
    engine->paused = true;
    while (engine->paused){
        // Wait until not in the middle of processing
        // TODO all usleeps need to be addressed for speedup
        usleep(1000); // Sleep for 1 ms

        // TODO again this is specific to QEMU and how sleeping is affected in ICOUNT mode, make it more generalized later
        engine->pause_status_cb(engine->pause_status_opaque);
    }
}

void pdes_play(void *opaque){
    PDESEngine *engine = opaque;
    engine->paused = false;
}


int pdes_drain(PDESEngine *engine, char * snapshot_name) {
    // TODO list of things that should be turned off when no sync is needed
    // TODO turn this based on connected neighbours later
    // if (!engine->needs_sync){
    //     // If sync is not needed, no drain is needed
    //     return 0;
    // }
    engine->checkpoint_in_progress = true;
    // Not putting drained to false as we might have already recieved it

    // Create a message for drain start, with the time being current virtual time
    // letting others know we are done with our own drain and waiting for their messages
    // set data as snapshot name
    uint8_t snapshot_name_data[1000];
    // Name is QPDES + snapshot name
    size_t snapshot_name_len = strnlen(snapshot_name, sizeof(snapshot_name_data)) + 5;
    snprintf((char *)snapshot_name_data, sizeof(snapshot_name_data), "QPDES%s", snapshot_name);
    Message drain_start_msg = create_message(snapshot_name_data, snapshot_name_len, DRAIN_START, get_universal_virtual_time(engine));
    pdes_comm_send(engine->comm, &drain_start_msg);
    printf("created drain start message with snapshot name: %s\n", snapshot_name_data);


    printf("PDES Engine starting drain process...\n");

    while (engine->neighbour_drained == false){
        // Send drain start message repeatedly until drained is true
        usleep(1000); // Sleep for 1 ms
        pdes_engine_poll(engine);
    }

    printf("PDES Engine drain process completed.\n");

    // TODO IMPORTANT move this to after checkpoint is done
    Message drain_end_msg = create_message(NULL, 0, DRAIN_END, get_universal_virtual_time(engine));
    pdes_comm_send(engine->comm, &drain_end_msg);
    engine->neighbour_drained = false;

    return 0;
}
