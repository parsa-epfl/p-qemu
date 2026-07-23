=============================================
Delayed-Response VirtIO Block Device Design
=============================================

Overview
========

The delayed-response VirtIO block device (``delay-virtio-blk``) is a variation
of the standard ``virtio-blk`` device.  From the guest driver's perspective it
is indistinguishable from a normal VirtIO block device — same device ID
(``VIRTIO_ID_BLOCK = 2``), same config space, same virtqueue protocol.

The difference is in the **host-side completion timing**: every request
completes exactly *N* microseconds after submission, provided the real
underlying block I/O finishes within that window.  If the real I/O takes
longer, the guest is stalled until the real I/O finishes.  The guest never
receives a completion before the real data is available.

Assumptions and Scope
---------------------

* **No dataplane.**  The device runs entirely in the QEMU main loop thread
  under BQL (Big QEMU Lock).  This is the natural mode for TCG guests and
  keeps the timer model simple — ``aio_poll()`` blocks the entire VM when the
  real I/O is slower than the configured latency.

* **TCG-first.**  While the design does not preclude KVM or dataplane, those
  are explicitly out of scope for the initial implementation.

* **MMIO transport only.**  The device plugs into the existing
  ``virtio-mmio`` bus, which requires no modifications.  PCI transport is
  trivially supported by the ``virtio-blk-pci`` wrapper if needed later.


Architecture
============

QOM Type Hierarchy
------------------

::

    TYPE_DEVICE
      └── TYPE_VIRTIO_DEVICE
            ├── TYPE_VIRTIO_BLK          ("virtio-blk-device")   — existing
            └── TYPE_DELAY_VIRTIO_BLK    ("delay-virtio-blk")    — new

The new type registers the same device ID (``VIRTIO_ID_BLOCK = 2``) but has
its own ``class_init``, ``realize``, and properties.

Transport
---------

The device plugs into the **standard virtio-mmio bus** without any transport
changes.  The MMIO transport (``hw/virtio/virtio-mmio.c``) is generic — it
routes register accesses to any ``VirtIODevice`` subclass and delivers
interrupts via ``qemu_set_irq(proxy->irq)``.

Because dataplane is disabled:

* ``virtio_device_ioeventfd_enabled(vdev)`` always returns ``false``.
* ``virtio_blk_data_plane_create()`` returns immediately with ``*dataplane = NULL``.
* ``s->dataplane_started`` is always ``false``.
* The completion path always takes the ``virtio_notify(vdev, vq)`` branch (not
  ``virtio_notify_irqfd()``).
* All processing runs in the QEMU main loop thread under BQL.


Timer Model (Core Design)
=========================

Philosophy
----------

The timer models the **total end-to-end response time** of a storage device.
It is started at request submission, not at I/O completion.  The real I/O
runs asynchronously in the background; the timer is the *only* source of
completions.

State Machine Per Request
-------------------------

::

                  SUBMITTED
                 (T = now)
                      │
         timer armed  │  real I/O dispatched
         for deadline │  (blk_aio_preadv / blk_aio_pwritev / …)
                      │
          ┌───────────┴───────────┐
          ▼                       ▼
    ┌──────────┐            ┌──────────┐
    │  Timer   │            │   I/O    │
    │  fires   │            │ completes│
    └────┬─────┘            └────┬─────┘
         │                       │
         ▼                       ▼
  req->timer_done = true   req->io_done = true
         │                 req->io_status = status
         │                       │
         ├─ io_done? ──YES───────┤
         │   ↓ complete_now()    │
         │                       │
         └─ io_done? ──NO────────┤
             ↓                   │
        aio_poll(ctx, true)      │
        (block until I/O done)   │
             │                   │
             └───────DONE────────┘

Two scenarios arise naturally:

==============  ============================================================
Scenario         What happens
==============  ============================================================
I/O fast         Real I/O finishes first → ``io_done`` flag set.
                 Timer fires at deadline → sees ``io_done == true``
                 → completes immediately.

I/O slow         Timer fires at deadline → sees ``io_done == false``
                 → calls ``aio_poll(ctx, true)``, which blocks the
                 event loop until the real I/O callback sets
                 ``io_done``.  Then completes.
==============  ============================================================

The **I/O completion callback never completes a request on its own**.
It is a pure flag-setter:

.. code-block:: c

    static void delay_virtio_blk_rw_complete(void *opaque, int ret)
    {
        DelayVirtIOBlockReq *req = opaque;
        req->io_done = true;
        req->io_status = (ret == 0) ? VIRTIO_BLK_S_OK : VIRTIO_BLK_S_IOERR;
    }

The **timer callback** is the sole completion driver:

.. code-block:: c

    static void delay_completion_timer_cb(void *opaque)
    {
        DelayVirtIOBlock *s = opaque;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

        while (!QTAILQ_EMPTY(&s->delay_queue)) {
            DelayVirtIOBlockReq *req = QTAILQ_FIRST(&s->delay_queue);
            if (req->deadline_ns > now) {
                /* Re-arm timer for next deadline. */
                timer_mod(s->delay_timer, req->deadline_ns);
                break;
            }
            QTAILQ_REMOVE(&s->delay_queue, req, queue_link);

            /* Block until real I/O finishes if it hasn't yet. */
            while (!req->io_done) {
                aio_poll(blk_get_aio_context(s->blk), true);
            }

            virtio_blk_req_complete(req, req->io_status);
            block_acct_done(…, &req->acct);
            virtio_blk_free_request(req);
        }
    }

Clock Choice: ``QEMU_CLOCK_VIRTUAL``
-----------------------------------

We use ``QEMU_CLOCK_VIRTUAL`` for the deadline timer.  This means the
guest-visible I/O latency is measured in **simulated (virtual) time**, not
wall-clock time.  Under TCG, the virtual clock advances proportionally to
guest instruction execution.

The timer is created with::

    timer_new_ns(QEMU_CLOCK_VIRTUAL, delay_completion_timer_cb, s);

and armed with a virtual-time deadline::

    int64_t deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns;
    timer_mod(s->delay_timer, deadline);

Why not ``QEMU_CLOCK_REALTIME``?

* With ``QEMU_CLOCK_REALTIME``, the guest would experience host-dependent
  wall-clock latency.  A heavily-loaded host would cause the guest to see
  artificially long I/O times even though the simulated device is fast.
  ``QEMU_CLOCK_VIRTUAL`` gives deterministic, reproducible guest-visible
  timing independent of host load.

* For research and simulation (the target use case), virtual-time determinism
  is essential for reproducible experiments.

Virtual Time Freeze Under Slow I/O
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

When the real I/O is slower than the configured delay, the timer callback
calls ``aio_poll(ctx, true)`` to wait.  During this wait, virtual time is
**frozen**.  This is *correct behavior*, not a bug: from the guest's
perspective, the I/O is not yet complete, so the guest should not advance.

Crucially, ``aio_poll()`` does not deadlock — it processes events on the
same ``AioContext``, including the block I/O completion event that will
set ``req->io_done = true`` and allow the loop to exit.

Timer Context: Interaction with Quantum-Based TCG Execution
-----------------------------------------------------------

This project uses two custom TCG accelerator operations that replace QEMU's
standard main-loop-driven timer processing:

1. **Quantum Round-Robin** (``tcg-accel-ops-quantum-rr.c``):
   A single TCG thread executes all vCPUs in round-robin order.  After each
   round, it calls ``increase_quantum_time()`` followed by
   ``qemu_clock_run_timers(QEMU_CLOCK_VIRTUAL)`` to fire expired timers.
   Our timer callback runs in **this TCG thread**.

2. **Quantum Parallel** (``tcg-accel-ops-quantum.c``):
   Multiple TCG threads (one per vCPU) independently execute their quantum
   budgets, then synchronize at a global barrier
   (``dynamic_barrier_polling_wait``).  The last thread to reach the barrier
   processes pending timers before releasing all threads for the next
   quantum.  Our timer callback runs in the **barrier-processing thread**.

In both models, when our timer callback blocks in ``aio_poll()``:

* **Quantum-RR**: The single TCG thread blocks.  No vCPU executes.  The
  quantum round does not advance.  Virtual time is frozen across all vCPUs.

* **Quantum Parallel**: The thread handling the timer (last-to-barrier)
  blocks inside ``aio_poll()``.  The barrier is never released because one
  thread has not left it.  All other vCPU threads spin at the barrier.
  Virtual time is frozen across all vCPUs.

In both cases, the guest experiences a single, atomic I/O completion at the
virtual-time deadline.  No other virtual timers fire, no other devices make
progress, and no guest instructions execute during the wait.  Once the real
I/O completes (``io_done`` is set), the timer callback exits,
``qemu_clock_run_timers()`` returns, and execution resumes normally.

Future Optimization: Per-vCPU Interrupt Awareness
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

Currently, when ``aio_poll()`` blocks in the parallel model, **all** vCPUs
stall at the barrier.  An optimization would be to:

1. Determine which vCPU the VirtIO interrupt targets (via the platform's
   interrupt controller routing — e.g., GIC affinity).
2. Block only that vCPU's thread (by having it spin-wait on ``io_done``
   without holding the barrier).
3. Allow other vCPUs to pass the barrier and continue executing their next
   quantum normally.

This requires exposing interrupt routing information to the device model,
which is not straightforward in QEMU's current architecture.  It is noted
as future work.

Timer vs I/O Context
--------------------

Without dataplane, the block backend's ``AioContext`` is the QEMU main loop
context (``qemu_get_aio_context()``), which is shared with the TCG thread
in single-threaded mode.  ``aio_poll(ctx, true)`` in the timer callback
processes events on this context, including the block I/O completion that
unblocks the wait.


Latency Tiers
=============

The device exposes three configurable delays:

==============  ==============  ============================================
Property         Default         Meaning
==============  ==============  ============================================
``short-delay-us``  50 µs       Delay for requests that involve **no block
                                I/O**: ``VIRTIO_BLK_T_GET_ID``, invalid
                                requests, unsupported commands.

``long-delay-us``   500 µs      Base delay for requests that **do** involve
                                block I/O: reads, writes, flushes, discards,
                                write-zeroes, zone operations.  Applied to
                                the entire batch for merged requests.
                                Note: for the SCSI command, it also involves
                                long delay.

``merge-overhead-us`` 30 µs     Additional delay **per merged request**
                                in a batch.  A batch of *N* merged requests
                                completes at::

                                    deadline = now + long_delay + N × merge_overhead

                                All *N* requests within the batch complete
                                simultaneously at that deadline.
==============  ==============  ============================================

Operation Classification
------------------------

==============  ==========  ================================================
Operation        Tier        Notes
==============  ==========  ================================================
``T_IN`` /       long        Standard read/write.  May be merged.
``T_OUT``
``T_FLUSH``      long        Cache flush.  Never merged.
``T_DISCARD``    long        Discard / unmap.
``T_WRITE_ZEROES`` long     Write zeroes.
``T_ZONE_*``     long        All zoned-device operations.
``T_SCSI_CMD``   long        SCSI passthrough (if feature negotiated).
``T_GET_ID``     short       Serial number query — no I/O.
Invalid/unsupp   short       Immediate error — no I/O.
==============  ==========  ================================================


Data Structures
===============

Device State
------------

.. code-block:: c

    struct DelayVirtIOBlock {
        VirtIODevice parent_obj;
        BlockBackend *blk;
        VirtIOBlkConf conf;              /* reuse from virtio-blk.h */
        unsigned short sector_mask;
        bool original_wce;
        VMChangeStateEntry *change;
        uint64_t host_features;
        size_t config_size;
        BlockRAMRegistrar blk_ram_registrar;

        /* Delay-specific fields */
        QEMUTimer *delay_timer;          /* one-shot, armed to earliest deadline */
        QTAILQ_HEAD(, DelayVirtIOBlockReq) delay_queue;  /* sorted by deadline_ns */

        /* Configurable properties */
        uint64_t short_delay_us;
        uint64_t long_delay_us;
        uint64_t merge_overhead_us;
    };

Per-Request State
-----------------

.. code-block:: c

    struct DelayVirtIOBlockReq {
        /* Existing virtio-blk fields (same layout as VirtIOBlockReq) */
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
        bool timer_done;                      /* deadline has passed */
        uint8_t io_status;                    /* saved completion status */
        QTAILQ_ENTRY(DelayVirtIOBlockReq) queue_link;  /* delay queue entry */
    };

Key Points
----------

* ``deadline_ns`` is set at **submission time** to
  ``qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns``.

* The ``delay_queue`` is kept sorted by ``deadline_ns`` (ascending).  New
  entries are inserted at the correct position so the head is always the
  earliest deadline.

* The single ``delay_timer`` is always armed to the head's ``deadline_ns``.
  When the timer fires, it drains all entries whose deadline has passed,
  re-arming for the next if any remain.

* ``io_status`` stores whether the real I/O succeeded (``VIRTIO_BLK_S_OK``)
  or failed (``VIRTIO_BLK_S_IOERR``), set by the I/O callback.

* We eliminate ``dataplane_started``, ``dataplane_disabled``, ``rq`` (error
  queue), and ``struct VirtIOBlockDataPlane *dataplane`` from the device
  struct since dataplane is not supported initially.


Request Lifecycle (Complete Flow)
=================================

Submission
----------

1. Guest writes ``VIRTIO_MMIO_QUEUE_NOTIFY`` →
   ``virtio_mmio_write()`` →
   ``virtio_queue_notify(vdev, n)`` →
   ``delay_virtio_blk_handle_output()`` →
   ``delay_virtio_blk_handle_vq()``.

2. ``delay_virtio_blk_handle_vq()`` pops requests from the virtqueue, calls
   ``delay_virtio_blk_handle_request()`` for each.

3. ``delay_virtio_blk_handle_request()`` classifies the request:

   * **Short-tier** (GET_ID, invalid): computes ``deadline_ns = now +
     short_delay_ns``, inserts into ``delay_queue``, arms timer.  No block
     I/O.  Sets ``io_done = true`` immediately (since there is no I/O to
     wait for).

   * **Long-tier** (read/write/flush/discard/zone): buffers into
     ``MultiReqBuffer`` as before.  When the batch is submitted:

     a. ``deadline_ns = now + long_delay_ns + (num_reqs × merge_overhead_ns)``
     b. Each request in the batch gets the **same** ``deadline_ns``
     c. Each request inserted into ``delay_queue`` with ``io_done = false``
     d. Real I/O dispatched via ``blk_aio_*()`` — no deadline computation is
        changed further.

Real I/O Completion
-------------------

The real I/O callback (e.g., ``delay_virtio_blk_rw_complete``) fires at some
future time:

* Sets ``req->io_done = true``.
* Sets ``req->io_status`` based on the I/O return code.
* Does **not** call ``virtio_blk_req_complete()``.
* Does **not** touch the timer or the queue.

Guest memory for reads is already updated at this point (the block layer
wrote directly into the guest's scatter-gather buffers during the I/O).  The
status byte in the VirtIO ``inhdr`` is *not yet written* — that happens
during ``virtio_blk_req_complete()`` when the timer fires.

Timer Firing
------------

The timer callback ``delay_completion_timer_cb()`` runs:

1. Reads ``now`` from ``QEMU_CLOCK_VIRTUAL``.
2. While ``delay_queue`` head's ``deadline_ns <= now``:

   a. Removes head from queue.
   b. If ``!req->io_done``: calls ``aio_poll(ctx, true)`` in a loop until
      ``io_done`` becomes true (blocking the guest).
   c. Calls ``virtio_blk_req_complete(req, req->io_status)``.
   d. Frees ``req``.

3. If queue is non-empty, re-arms timer to new head's ``deadline_ns``.

Completion (virtio_blk_req_complete)
------------------------------------

The final completion step is identical to the existing one, except the
dataplane check always takes the ``virtio_notify()`` branch:

.. code-block:: c

    static void virtio_blk_req_complete(DelayVirtIOBlockReq *req, uint8_t status)
    {
        DelayVirtIOBlock *s = req->dev;

        stb_p(&req->in->status, status);           /* write status to guest */
        iov_discard_undo(&req->inhdr_undo);
        iov_discard_undo(&req->outhdr_undo);
        virtqueue_push(req->vq, &req->elem, req->in_len);
        virtio_notify(VIRTIO_DEVICE(s), req->vq);  /* fire IRQ */
    }


Merge Handling in Detail
========================

What Gets Merged
----------------

QEMU merges sequential, same-direction read or write requests into a single
``preadv`` / ``pwritev`` system call.  The merge logic (in
``delay_virtio_blk_submit_multireq``, adapted from the existing
``virtio_blk_submit_multireq``) is unchanged.

Deadline Assignment for Merged Batches
--------------------------------------

When ``submit_requests()`` dispatches a batch of *N* merged requests:

.. code-block:: c

    int64_t deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)
                     + s->long_delay_ns
                     + num_reqs * s->merge_overhead_ns;

    for (int i = start; i < start + num_reqs; i++) {
        DelayVirtIOBlockReq *req = mrb->reqs[i];
        req->deadline_ns = deadline;        /* all same */
        req->io_done = false;
        req->io_status = VIRTIO_BLK_S_OK;
        delay_queue_insert_sorted(&s->delay_queue, req);
    }

All *N* requests share the same deadline and will be drained together when
the timer fires.  ``delay_completion_timer_cb()`` walks them in merge order
and completes each.

Interaction with Error Handling
-------------------------------

If the real I/O fails (``ret < 0`` in the completion callback),
``io_status`` is set to ``VIRTIO_BLK_S_IOERR``.  The existing error-action
logic (stop / report / ignore) must be evaluated **before** queueing for
delayed completion.  Specifically:

* If ``BLOCK_ERROR_ACTION_STOP``: the request is held on ``s->rq`` (error
  queue) and the VM is stopped — it does NOT enter the delay queue.  This
  matches the existing ``virtio_blk_handle_rw_error()`` behavior.

* If ``BLOCK_ERROR_ACTION_REPORT`` or ``IGNORE``: the request proceeds to
  the delay queue with the appropriate error status.  The guest still
  experiences the configured delay before seeing the error.

Actually, this means the error handling must happen in the I/O completion
callback (before setting ``io_done``), not in the timer callback.  The I/O
callback should:

.. code-block:: c

    static void delay_virtio_blk_rw_complete(void *opaque, int ret)
    {
        DelayVirtIOBlockReq *next = opaque;
        while (next) {
            DelayVirtIOBlockReq *req = next;
            next = req->mr_next;

            if (ret) {
                if (delay_virtio_blk_handle_rw_error(req, -ret, …)) {
                    /* Error action STOP: req moved to error queue, skip delay */
                    continue;
                }
                req->io_status = VIRTIO_BLK_S_IOERR;
            } else {
                req->io_status = VIRTIO_BLK_S_OK;
            }
            req->io_done = true;
        }
    }


Completion Callbacks to Modify
==============================

Every async completion callback in the original ``virtio-blk.c`` must be
adapted to set ``io_done`` / ``io_status`` instead of calling
``virtio_blk_req_complete()`` directly:

========================================  ===================================
Original callback                         Adapted behavior
========================================  ===================================
``virtio_blk_rw_complete()``              Walks merge chain, handles errors,
                                          sets ``io_done``, does NOT complete.
``virtio_blk_flush_complete()``           Sets ``io_done``.
``virtio_blk_discard_write_zeroes_complete()``  Sets ``io_done``.
``virtio_blk_zone_report_complete()``     Sets ``io_done``.
``virtio_blk_zone_mgmt_complete()``       Sets ``io_done``.
``virtio_blk_zone_append_complete()``     Sets ``io_done``.
``virtio_blk_ioctl_complete()``           Sets ``io_done``.
========================================  ===================================

Synchronous completions (``T_GET_ID``, errors caught at parse time) are
modified to insert into the delay queue with ``io_done = true`` and a
short-tier deadline, rather than completing inline.


Error Queue (s->rq)
===================

The existing error queue mechanism (``s->rq`` linked list) is retained for
``BLOCK_ERROR_ACTION_STOP``.  When the VM is resumed,
``delay_virtio_blk_dma_restart_cb()`` (called from the VM change state
handler) re-processes the queued requests.  These re-processed requests
should receive a **fresh deadline** (i.e., the configured delay starts from
the resume time, not the original submission time).


Configuration
=============

QEMU Command Line
-----------------

::

    -device delay-virtio-blk,drive=mydrive,short-delay-us=50,long-delay-us=500,merge-overhead-us=30

All delay properties are optional and have defaults (see Latency Tiers
table above).  The device accepts all standard ``virtio-blk`` properties
(drive, serial, num-queues, queue-size, etc.).

QOM Properties
--------------

.. code-block:: c

    static Property delay_virtio_blk_properties[] = {
        /* Standard virtio-blk properties */
        DEFINE_BLOCK_PROPERTIES(DelayVirtIOBlock, conf.conf),
        DEFINE_BLOCK_ERROR_PROPERTIES(DelayVirtIOBlock, conf.conf),
        DEFINE_PROP_STRING("serial", DelayVirtIOBlock, conf.serial),
        DEFINE_PROP_BIT("request-merging", DelayVirtIOBlock, conf.request_merging, 0, true),
        DEFINE_PROP_UINT16("num-queues", DelayVirtIOBlock, conf.num_queues, 1),
        DEFINE_PROP_UINT16("queue-size", DelayVirtIOBlock, conf.queue_size, 256),
        /* … other standard props as needed … */

        /* Delay-specific properties */
        DEFINE_PROP_UINT64("short-delay-us", DelayVirtIOBlock, short_delay_us, 50),
        DEFINE_PROP_UINT64("long-delay-us",  DelayVirtIOBlock, long_delay_us,  500),
        DEFINE_PROP_UINT64("merge-overhead-us", DelayVirtIOBlock, merge_overhead_us, 30),

        DEFINE_PROP_END_OF_LIST(),
    };


Thread Safety
=============

With dataplane disabled, thread safety is straightforward:

* All request processing, I/O submission, timer callbacks, and I/O
  completion callbacks run in the QEMU main loop thread under BQL.

* ``aio_poll(ctx, true)`` in the timer callback runs a nested event loop.
  During this nested poll, only events for the main loop ``AioContext`` are
  processed.  The BQL is dropped and re-acquired around each event dispatch
  by the event loop machinery.

* The ``delay_queue`` is only accessed from the main loop thread; no
  locking is needed.

* ``qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL)`` is safe to call from the
  TCG thread (which holds the BQL in single-threaded mode).

Future Dataplane Support
------------------------

Adding dataplane support would require:

* Creating the timer with ``aio_timer_new(iothread_ctx, …)`` instead of
  ``timer_new_ns(…)``.
* Protecting ``delay_queue`` with a mutex (since I/O completions arrive in
  the IOThread while timer fires in the IOThread — actually they share the
  same ``AioContext``, so it may not need locking).
* Using ``virtio_notify_irqfd()`` instead of ``virtio_notify()`` in the
  completion path.
* Ensuring ``aio_poll(ctx, true)`` only blocks the IOThread, not the main
  loop.

This is deferred to a future revision.


Files to Create / Modify
========================

New Files
---------

``hw/block/delay-virtio-blk.c``
    Main device implementation.  A copy of ``hw/block/virtio-blk.c`` with:

    * Renamed types and functions (``VirtIOBlock`` → ``DelayVirtIOBlock``, etc.)
    * Added timer, queue, and deadline logic
    * Modified completion callbacks (flag-setter pattern)
    * Modified ``virtio_blk_handle_request()`` (short vs long tier dispatch)
    * Removed dataplane initialization from ``realize()``
    * Added delay-specific properties
    * Type registration as ``TYPE_DELAY_VIRTIO_BLK``

``include/hw/virtio/delay-virtio-blk.h``
    Header defining ``DelayVirtIOBlock``, ``DelayVirtIOBlockReq``, the type
    macros, and the ``DelayVirtIOBlkConf`` (if needed beyond the existing
    ``VirtIOBlkConf``).

Modified Files
--------------

``hw/block/meson.build``
    Add the new source file:

    .. code-block:: python

        specific_ss.add(when: 'CONFIG_VIRTIO_BLK', if_true: files(
            'virtio-blk.c',
            'virtio-blk-common.c',
            'delay-virtio-blk.c',          # ← added
        ))

    Alternatively, gate it behind its own Kconfig option.

``hw/block/Kconfig``
    Optionally add a ``CONFIG_DELAY_VIRTIO_BLK`` config symbol that selects
    ``CONFIG_VIRTIO_BLK``.

No Transport Changes
--------------------

``hw/virtio/virtio-mmio.c`` requires **no modifications**.  It already
handles any ``VirtIODevice`` subclass generically.  The device is
instantiated the same way as regular ``virtio-blk``:

::

    -device virtio-mmio,addr=0x0a000000
    -device delay-virtio-blk,drive=mydrive

Or, for machine types that auto-create MMIO transports:

::

    -device delay-virtio-blk,drive=mydrive


Open Questions
==============

1. **Error action STOP and delay.**  When the VM is stopped due to an I/O error
   and later resumed, should the resumed requests use their original deadline,
   a new deadline from resume time, or bypass the delay entirely?  The
   proposed answer: **new deadline from resume time**.

2. **SCSI passthrough.**  ``VIRTIO_BLK_T_SCSI_CMD`` goes through a different
   code path (``virtio_blk_handle_scsi_req``).  Should it use the long delay
   tier?  Proposed: **yes**, long delay tier, since it involves real I/O.

3. **Multiple queues.**  The existing virtio-blk supports ``num_queues > 1``.
   With a single timer and queue, all virtqueues share the same delay queue.
   This is fine: the timer always fires at the earliest deadline across all
   queues.  Is this acceptable, or should each virtqueue have its own timer
   and queue?

4. **Migration.**  Not supported in the initial implementation.  The delay
   queue state would need to be serialized.  A future revision could add
   ``VMStateDescription`` fields for the timer and queue contents.


Summary
=======

The delayed-response VirtIO block device provides a drop-in replacement for
``virtio-blk`` that models storage devices with configurable, constant
response times.  The design leverages QEMU's existing async I/O
infrastructure, adding a deadline-ordered queue and a single virtual-time timer
to gate completions.  When the real I/O is slower than the configured delay,
the guest VM is paused via ``aio_poll()`` until the I/O finishes, ensuring
real data integrity.  Under TCG with quantum-based scheduling, virtual time
freezes during the wait — no vCPU makes progress, no other timers fire, and
the guest sees a single atomic I/O completion at the virtual-time deadline.
