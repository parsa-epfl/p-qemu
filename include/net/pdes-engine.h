#ifndef NET_PDES_ENGINE_H
#define NET_PDES_ENGINE_H

#include <stdint.h>
#include <stddef.h>
#include "pdes-communicator.h"
#include "qemu/timer.h"

typedef struct PDESEngine PDESEngine;
typedef struct PDESWWT PDESWWT;


typedef void (*PDESFinalRecvCallback)(void *opaque, const uint8_t *data, size_t len);
typedef void (*PDESRecvCallback)(void *opaque, Message *msg);
// TODO This might be too QEMU specific, think about it in terms of general soltions later
typedef void (*PauseStatusCallBack)(void *opaque);


struct PDESEngine {
    PDESCommunicator *comm;
    bool needs_sync;
    uint64_t latencyns;
    PDESRecvCallback recv_cb;
    void *recv_opaque;
    QEMUTimer *msg_rec_poll_timer;
    QEMUTimer *sync_poll_timer;
    QEMUTimer *setup_poll_timer;
    bool has_first_sync;
    bool pair_has_finished;

    // Specific to QEMU implications of blocking event queu in case of pause on device and icount TODO generalize
    PauseStatusCallBack pause_status_cb;
    void *pause_status_opaque;

    bool paused;


    uint64_t base_diff;
    uint64_t first_sync_time;

    

    // WWT specific
    bool waiting_for_quanta;
};

PDESEngine *pdes_engine_create(
    const char *shm_send, 
    const char *shm_recv, 
    bool sync, 
    uint64_t latencyns,
    PDESRecvCallback cb, 
    void *opaque,
    PauseStatusCallBack pause_status_cb,
    void *pause_status_opaque
);
void pdes_engine_destroy(PDESEngine *engine);
int pdes_engine_send(PDESEngine *engine, const uint8_t *data, size_t len);
void pdes_engine_poll(void *opaque);

// TODO this needs to move to a proper library and its own thread
void schedule_poll(void *opaque);


void pdes_pause(void *opaque);
void pdes_play(void *opaque);






// WWT specific: TODO move to its own headr file later
struct PDESWWT{
    PDESEngine *engine;
    uint64_t quantum_ns;
    uint64_t number_of_neighbors;
    uint64_t number_of_neighbors_finished;
    bool has_finished;
    PDESFinalRecvCallback recv_cb;
    void *recv_opaque;
    bool should_sync;

    QEMUTimer *setup_timer;
    QEMUTimer *quantum_timer;
};

PDESWWT *pdes_engine_wwt_create(
    const char *shm_send,
    const char *shm_recv,
    bool sync,
    uint64_t latencyns,
    PDESFinalRecvCallback cb, 
    void *opaque
);
void setup_wwt(PDESWWT *wwt_engine);
void send_sync(PDESWWT *wwt_engine);
void finish_quantum(PDESWWT *wwt_engine);
int wwt_send(PDESWWT *wwt_engine, const uint8_t *data, size_t len);
// TODO put its type to PDESRecvCallback
void wwt_recivied_callback(void *opaque, Message *msg);
bool is_waiting_for_quanta(PDESWWT *wwt_engine);





#endif