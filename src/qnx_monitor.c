/*
 * qnx_monitor.c
 *
 * Periodic telemetry sampler for QNX Neutrino 8.0.
 *
 * This is a small teaching example of a fixed-rate sampling loop, the kind
 * of pattern used for periodic sensor reads in robotics and medical-device
 * style workloads. It is a demo, not a certified or safety-rated component,
 * and it makes no real-time guarantees.
 *
 * What it shows:
 *   1. Absolute-deadline timing. The sampler thread sleeps with
 *      clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, ...) until a deadline
 *      that advances by exactly SAMPLE_PERIOD_NS each cycle. Because the
 *      deadline is absolute, time spent doing work does not accumulate as
 *      drift the way a relative sleep (delay(), nanosleep()) does.
 *   2. QNX native message passing. The sampler runs as a SCHED_FIFO thread
 *      and sends each sample to a normal-priority logger thread with
 *      MsgSend(). The logger receives with MsgReceive(), prints, and answers
 *      with MsgReply(). All printing happens in the logger, so the sampler
 *      thread does no console I/O inside its loop.
 *
 * Threads:
 *   main     default priority; sets things up, handles SIGINT/SIGTERM,
 *            waits for the other two threads.
 *   sampler  SCHED_FIFO, priority RT_PRIORITY; takes samples on schedule.
 *   logger   default priority; owns the channel, prints samples and the
 *            end-of-run summary.
 *
 * Missed-deadline policy:
 *   After each sample is sent, the sampler advances the deadline by one
 *   period. If the current time is already past that deadline, the deadline
 *   is counted as missed. The sampler then skips ahead to the next period
 *   that is still in the future, counting every skipped period as missed,
 *   so it never "bursts" to catch up. Skipped periods make the next
 *   reported interval longer than one period.
 *
 * Note: MsgSend() is synchronous. The sampler stays blocked until the logger
 * calls MsgReply(), so each send adds a short, variable delay to the sampler
 * thread. See the README "Limits" section for the pulse alternative.
 *
 * Usage:   qnx_monitor [samples]
 *          samples defaults to DEFAULT_SAMPLES; 0 means run until stopped.
 * Stop:    Ctrl+C, or kill -s INT <pid> (pid from pidin). The summary still prints.
 * Observe: pidin -p qnx_monitor
 *
 * Build:   make        (after running qnxsdp-env.bat / qnxsdp-env.sh)
 *
 * Sponsored by CREIGNIFICENT LLC
 */

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/iomsg.h>
#include <sys/neutrino.h>
#include <sys/netmgr.h>

#define NSEC_PER_SEC      1000000000LL
#define NSEC_PER_MSEC     1000000LL
#define SAMPLE_PERIOD_NS  (100LL * NSEC_PER_MSEC)  /* 100 ms target (10 Hz) */
#define DEFAULT_SAMPLES   100                      /* about 10 seconds     */
#define RT_PRIORITY       50                       /* SCHED_FIFO priority  */

/*
 * Message types. Values above _IO_MAX keep these private messages clear of
 * the range QNX reserves for resource-manager (I/O) messages.
 */
#define MSG_SAMPLE    (_IO_MAX + 1)
#define MSG_SHUTDOWN  (_IO_MAX + 2)

/* One sample, sent from the sampler to the logger. */
typedef struct {
    uint16_t type;            /* MSG_SAMPLE                                   */
    uint16_t reserved;
    uint32_t seq;             /* sample number, starting at 0                 */
    int64_t  t_ns;            /* CLOCK_MONOTONIC time of the sample, ns        */
    int64_t  interval_ns;     /* time since the previous sample (or start)    */
    int64_t  jitter_ns;       /* interval_ns - SAMPLE_PERIOD_NS               */
    int64_t  latency_ns;      /* how late the wake-up was vs. its deadline    */
    uint32_t skipped_before;  /* periods skipped just before this sample      */
    uint32_t pad;
    uint64_t cycles;          /* ClockCycles() since the sampler started      */
} sample_msg_t;

/* Sent once by the sampler when it is done; the logger prints the summary. */
typedef struct {
    uint16_t type;            /* MSG_SHUTDOWN                                 */
    uint16_t reserved;
    uint32_t samples_taken;
    uint32_t missed_deadlines;
    uint32_t stopped_by_signal;
} shutdown_msg_t;

/* Lets MsgReceive() take any of our message types into one buffer. */
typedef union {
    uint16_t       type;
    sample_msg_t   sample;
    shutdown_msg_t shutdown;
} any_msg_t;

static atomic_int stop_requested = 0;   /* set by the SIGINT/SIGTERM handler */
static atomic_int logger_released = 0;  /* set once MSG_SHUTDOWN is delivered */
static int        chid = -1;            /* channel owned by the logger       */
static long       max_samples = DEFAULT_SAMPLES;

/* ---------------------------------------------------------------- helpers */

static int64_t ts_to_ns(const struct timespec *ts)
{
    return (int64_t)ts->tv_sec * NSEC_PER_SEC + ts->tv_nsec;
}

/* Add ns to a timespec, keeping tv_nsec in [0, 1e9). */
static void ts_add_ns(struct timespec *ts, int64_t ns)
{
    int64_t total = (int64_t)ts->tv_nsec + ns;
    ts->tv_sec += (time_t)(total / NSEC_PER_SEC);
    total %= NSEC_PER_SEC;
    if (total < 0) {
        total += NSEC_PER_SEC;
        ts->tv_sec -= 1;
    }
    ts->tv_nsec = (long)total;
}

static double ns_to_ms(int64_t ns)
{
    return (double)ns / (double)NSEC_PER_MSEC;
}

static void on_stop_signal(int signo)
{
    (void)signo;
    atomic_store(&stop_requested, 1);
}

/* Sleep until an absolute CLOCK_MONOTONIC deadline. Retries on EINTR. */
static int sleep_until(const struct timespec *deadline)
{
    int rc;
    do {
        rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, NULL);
    } while (rc == EINTR && !atomic_load(&stop_requested));
    return (rc == EINTR) ? 0 : rc;   /* EINTR here only means "stop asked" */
}

/* Send the shutdown message on an existing connection. */
static int send_shutdown(int coid, uint32_t taken, uint32_t missed)
{
    shutdown_msg_t msg;
    memset(&msg, 0, sizeof msg);
    msg.type = MSG_SHUTDOWN;
    msg.samples_taken = taken;
    msg.missed_deadlines = missed;
    msg.stopped_by_signal = (uint32_t)atomic_load(&stop_requested);

    if (MsgSend(coid, &msg, sizeof msg, NULL, 0) == -1) {
        fprintf(stderr, "[SAMPLER] MsgSend(shutdown) failed: %s\n",
                strerror(errno));
        return -1;
    }
    atomic_store(&logger_released, 1);
    return 0;
}

/* Used by main if the sampler never delivered MSG_SHUTDOWN. */
static void shutdown_logger_from_main(void)
{
    int coid = ConnectAttach(ND_LOCAL_NODE, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        fprintf(stderr, "[MAIN] ConnectAttach failed: %s\n", strerror(errno));
        return;
    }
    (void)send_shutdown(coid, 0, 0);
    if (ConnectDetach(coid) == -1) {
        fprintf(stderr, "[MAIN] ConnectDetach failed: %s\n", strerror(errno));
    }
}

/* ----------------------------------------------------------- logger thread */

static void *logger_thread(void *arg)
{
    (void)arg;
    any_msg_t msg;
    int64_t min_iv = INT64_MAX, max_iv = 0, sum_iv = 0, max_abs_jit = 0;
    uint32_t count = 0;

    for (;;) {
        int rcvid = MsgReceive(chid, &msg, sizeof msg, NULL);
        if (rcvid == -1) {
            if (errno == EINTR) {
                continue;
            }
            fprintf(stderr, "[LOG] MsgReceive failed: %s\n", strerror(errno));
            return NULL;
        }
        if (rcvid == 0) {
            continue;   /* a pulse; this demo does not use pulses, ignore it */
        }

        if (msg.type == MSG_SAMPLE) {
            const sample_msg_t *s = &msg.sample;

            /* Reply first so the sampler is released as early as possible. */
            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "[LOG] MsgReply failed: %s\n", strerror(errno));
            }

            int64_t abs_jit = s->jitter_ns < 0 ? -s->jitter_ns : s->jitter_ns;
            if (s->interval_ns < min_iv) min_iv = s->interval_ns;
            if (s->interval_ns > max_iv) max_iv = s->interval_ns;
            if (abs_jit > max_abs_jit)   max_abs_jit = abs_jit;
            sum_iv += s->interval_ns;
            count++;

            printf("[LOG] #%4" PRIu32 " interval=%9.3f ms jitter=%+8.3f ms "
                   "wake_late=%7.3f ms skipped=%" PRIu32 " cycles=%" PRIu64 "\n",
                   s->seq, ns_to_ms(s->interval_ns), ns_to_ms(s->jitter_ns),
                   ns_to_ms(s->latency_ns), s->skipped_before, s->cycles);
            fflush(stdout);
        } else if (msg.type == MSG_SHUTDOWN) {
            const shutdown_msg_t *d = &msg.shutdown;

            printf("\n=== Summary (target period %.3f ms) ===\n",
                   ns_to_ms(SAMPLE_PERIOD_NS));
            printf("Samples taken     : %" PRIu32 "%s\n", d->samples_taken,
                   d->stopped_by_signal ? " (stopped by signal)" : "");
            if (count > 0) {
                printf("Interval min      : %.3f ms\n", ns_to_ms(min_iv));
                printf("Interval max      : %.3f ms\n", ns_to_ms(max_iv));
                printf("Interval avg      : %.3f ms\n",
                       ns_to_ms(sum_iv) / (double)count);
                printf("Max |jitter|      : %.3f ms\n", ns_to_ms(max_abs_jit));
            } else {
                printf("No samples were logged.\n");
            }
            printf("Missed deadlines  : %" PRIu32 "\n", d->missed_deadlines);
            fflush(stdout);

            if (MsgReply(rcvid, EOK, NULL, 0) == -1) {
                fprintf(stderr, "[LOG] MsgReply failed: %s\n", strerror(errno));
            }
            return NULL;
        } else {
            /* Unknown message: tell the sender instead of leaving it blocked. */
            if (MsgError(rcvid, ENOSYS) == -1) {
                fprintf(stderr, "[LOG] MsgError failed: %s\n", strerror(errno));
            }
        }
    }
}

/* ---------------------------------------------------------- sampler thread */

static void *sampler_thread(void *arg)
{
    (void)arg;
    struct timespec start, next, now;
    int64_t prev_ns;
    uint32_t seq = 0, missed = 0, skipped_before = 0;
    uint64_t cycles_start;
    int coid, rc;

    coid = ConnectAttach(ND_LOCAL_NODE, 0, chid, _NTO_SIDE_CHANNEL, 0);
    if (coid == -1) {
        fprintf(stderr, "[SAMPLER] ConnectAttach failed: %s\n", strerror(errno));
        return NULL;   /* main sends MSG_SHUTDOWN instead (see main) */
    }

    if (clock_gettime(CLOCK_MONOTONIC, &start) == -1) {
        fprintf(stderr, "[SAMPLER] clock_gettime failed: %s\n", strerror(errno));
        (void)send_shutdown(coid, 0, 0);
        (void)ConnectDetach(coid);
        return NULL;
    }
    cycles_start = ClockCycles();
    prev_ns = ts_to_ns(&start);
    next = start;
    ts_add_ns(&next, SAMPLE_PERIOD_NS);   /* first deadline: start + period */

    while (!atomic_load(&stop_requested) &&
           (max_samples == 0 || (long)seq < max_samples)) {

        rc = sleep_until(&next);
        if (rc != 0) {
            fprintf(stderr, "[SAMPLER] clock_nanosleep failed: %s\n",
                    strerror(rc));
            break;
        }
        if (atomic_load(&stop_requested)) {
            break;
        }

        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
            fprintf(stderr, "[SAMPLER] clock_gettime failed: %s\n",
                    strerror(errno));
            break;
        }

        /* Build the sample. A real system would read its sensor here. */
        sample_msg_t s;
        memset(&s, 0, sizeof s);
        int64_t now_ns = ts_to_ns(&now);
        s.type           = MSG_SAMPLE;
        s.seq            = seq;
        s.t_ns           = now_ns;
        s.interval_ns    = now_ns - prev_ns;
        s.jitter_ns      = s.interval_ns - SAMPLE_PERIOD_NS;
        s.latency_ns     = now_ns - ts_to_ns(&next);
        s.skipped_before = skipped_before;
        s.cycles         = ClockCycles() - cycles_start;
        prev_ns = now_ns;

        /* Synchronous: blocks until the logger replies. */
        if (MsgSend(coid, &s, sizeof s, NULL, 0) == -1) {
            fprintf(stderr, "[SAMPLER] MsgSend(sample) failed: %s\n",
                    strerror(errno));
            break;
        }
        seq++;

        /* Advance by exactly one period, then apply the missed policy. */
        ts_add_ns(&next, SAMPLE_PERIOD_NS);
        skipped_before = 0;
        if (clock_gettime(CLOCK_MONOTONIC, &now) == -1) {
            fprintf(stderr, "[SAMPLER] clock_gettime failed: %s\n",
                    strerror(errno));
            break;
        }
        int64_t late_ns = ts_to_ns(&now) - ts_to_ns(&next);
        if (late_ns > 0) {
            /* Count this deadline plus any further ones already passed. */
            int64_t skip = late_ns / SAMPLE_PERIOD_NS + 1;
            ts_add_ns(&next, skip * SAMPLE_PERIOD_NS);
            skipped_before = (uint32_t)skip;
            missed += (uint32_t)skip;
        }
    }

    (void)send_shutdown(coid, seq, missed);
    if (ConnectDetach(coid) == -1) {
        fprintf(stderr, "[SAMPLER] ConnectDetach failed: %s\n", strerror(errno));
    }
    return NULL;
}

/* -------------------------------------------------------------------- main */

static int parse_samples(const char *arg, long *out)
{
    char *end = NULL;
    errno = 0;
    long v = strtol(arg, &end, 10);
    if (errno != 0 || end == arg || *end != '\0' || v < 0 || v > 100000000L) {
        return -1;
    }
    *out = v;
    return 0;
}

int main(int argc, char *argv[])
{
    pthread_t logger_tid, sampler_tid;
    pthread_attr_t attr;
    struct sched_param param;
    struct sigaction sa;
    sigset_t stop_set, old_set;
    int rc, policy;

    if (argc > 2 || (argc == 2 && parse_samples(argv[1], &max_samples) != 0)) {
        fprintf(stderr, "Usage: %s [samples]   (0 = run until stopped, "
                "default %d)\n", argv[0], DEFAULT_SAMPLES);
        return EXIT_FAILURE;
    }

    printf("QNX periodic telemetry sampler\n");
    printf("Target period: %.3f ms | Samples: ", ns_to_ms(SAMPLE_PERIOD_NS));
    if (max_samples == 0) printf("until stopped\n");
    else                  printf("%ld\n", max_samples);
    printf("Stop early with Ctrl+C or kill -s INT <pid>; the summary still prints.\n\n");
    fflush(stdout);

    /* Handler only sets a flag. No SA_RESTART, so sleeps can see EINTR. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_stop_signal;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) == -1 ||
        sigaction(SIGTERM, &sa, NULL) == -1) {
        perror("sigaction");
        return EXIT_FAILURE;
    }

    /*
     * Block SIGINT/SIGTERM while creating the worker threads so they inherit
     * a mask that blocks them. Only main then receives these signals, which
     * keeps MsgSend/MsgReceive in the workers from being interrupted.
     */
    sigemptyset(&stop_set);
    sigaddset(&stop_set, SIGINT);
    sigaddset(&stop_set, SIGTERM);
    rc = pthread_sigmask(SIG_BLOCK, &stop_set, &old_set);
    if (rc != 0) {
        fprintf(stderr, "pthread_sigmask failed: %s\n", strerror(rc));
        return EXIT_FAILURE;
    }

    /* The channel the logger will receive on. */
    chid = ChannelCreate(0);
    if (chid == -1) {
        fprintf(stderr, "ChannelCreate failed: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    /* Logger: default attributes (normal priority). While it receives a
     * message, QNX runs it at the sender's priority, so pidin shows 50r. */
    rc = pthread_create(&logger_tid, NULL, logger_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_create(logger) failed: %s\n", strerror(rc));
        (void)ChannelDestroy(chid);
        return EXIT_FAILURE;
    }
    (void)pthread_setname_np(logger_tid, "logger");

    /* Sampler: explicit SCHED_FIFO at RT_PRIORITY. */
    if ((rc = pthread_attr_init(&attr)) != 0 ||
        (rc = pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED)) != 0 ||
        (rc = pthread_attr_setschedpolicy(&attr, SCHED_FIFO)) != 0) {
        fprintf(stderr, "pthread_attr setup failed: %s\n", strerror(rc));
        shutdown_logger_from_main();
        (void)pthread_join(logger_tid, NULL);
        (void)ChannelDestroy(chid);
        return EXIT_FAILURE;
    }
    memset(&param, 0, sizeof param);
    param.sched_priority = RT_PRIORITY;
    rc = pthread_attr_setschedparam(&attr, &param);
    if (rc == 0) {
        rc = pthread_create(&sampler_tid, &attr, sampler_thread, NULL);
    }
    (void)pthread_attr_destroy(&attr);
    if (rc != 0) {
        fprintf(stderr, "Could not start SCHED_FIFO sampler at priority %d: %s\n",
                RT_PRIORITY, strerror(rc));
        shutdown_logger_from_main();
        (void)pthread_join(logger_tid, NULL);
        (void)ChannelDestroy(chid);
        return EXIT_FAILURE;
    }
    (void)pthread_setname_np(sampler_tid, "sampler");

    /* Workers exist with signals blocked; now let main take them. */
    rc = pthread_sigmask(SIG_SETMASK, &old_set, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_sigmask restore failed: %s\n", strerror(rc));
    }

    if (pthread_getschedparam(sampler_tid, &policy, &param) == 0) {
        printf("[MAIN] Sampler thread running: priority=%d policy=%s\n",
               param.sched_priority,
               policy == SCHED_FIFO ? "FIFO" :
               policy == SCHED_RR   ? "RR"   : "OTHER");
        printf("[MAIN] Check it with: pidin -p qnx_monitor\n\n");
        fflush(stdout);
    }

    rc = pthread_join(sampler_tid, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_join(sampler) failed: %s\n", strerror(rc));
    }
    /* Normally the sampler already sent MSG_SHUTDOWN; if not, send it here. */
    if (!atomic_load(&logger_released)) {
        shutdown_logger_from_main();
    }
    /* Last resort so main never hangs: MsgReceive is a cancellation point. */
    if (!atomic_load(&logger_released)) {
        (void)pthread_cancel(logger_tid);
    }
    rc = pthread_join(logger_tid, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_join(logger) failed: %s\n", strerror(rc));
    }

    if (ChannelDestroy(chid) == -1) {
        fprintf(stderr, "ChannelDestroy failed: %s\n", strerror(errno));
    }

    printf("[MAIN] Done.\n");
    return EXIT_SUCCESS;
}
