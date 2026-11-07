/*
 * qnx_monitor.c
 *
 * Minimal real-time telemetry daemon for QNX Neutrino 8.0.
 *
 * Demonstrates:
 *   - Explicit high-priority thread creation (SCHED_FIFO)
 *   - Deterministic periodic sampling using ClockCycles / delay
 *   - Reporting of current priority, CPU cycles, and simple counters
 *
 * Build: make (after sourcing qnxsdp-env.sh)
 * Run on target: ./qnx_monitor
 * Observe with: pidin -p qnx_monitor
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>
#include <sys/neutrino.h>
#include <sys/syspage.h>

#define SAMPLE_INTERVAL_MS  100   /* 10 Hz telemetry */
#define NUM_SAMPLES         30    /* run for ~3 seconds */
#define RT_PRIORITY         50    /* high but not privileged (1-63 for non-root) */

static volatile int running = 1;

/* Real-time worker thread */
void *telemetry_thread(void *arg)
{
    (void)arg;
    uint64_t cycles_start, cycles_now;
    struct timespec ts;
    int sample = 0;

    /* Confirm we are running at the requested priority */
    struct sched_param param;
    int policy;
    pthread_getschedparam(pthread_self(), &policy, &param);

    printf("[RT] Telemetry thread started. Priority=%d Policy=%s\n",
           param.sched_priority,
           (policy == SCHED_FIFO) ? "FIFO" :
           (policy == SCHED_RR)   ? "RR"   : "OTHER");

    cycles_start = ClockCycles();

    while (running && sample < NUM_SAMPLES) {
        cycles_now = ClockCycles();
        clock_gettime(CLOCK_MONOTONIC, &ts);

        /* Simple telemetry payload */
        printf("[RT] Sample %2d | prio=%2d | cycles=%llu | mono=%ld.%09ld\n",
               sample,
               param.sched_priority,
               (unsigned long long)(cycles_now - cycles_start),
               (long)ts.tv_sec, ts.tv_nsec);

        sample++;

        /* Precise sleep (QNX delay() is reliable for this demo) */
        delay(SAMPLE_INTERVAL_MS);
    }

    printf("[RT] Telemetry thread finished (%d samples).\n", sample);
    return NULL;
}

int main(void)
{
    pthread_t rt_tid;
    pthread_attr_t attr;
    struct sched_param param;
    int rc;

    printf("QNX Real-Time Telemetry Monitor\n");
    printf("Target: Neutrino 8.0 | Sample rate: %d ms\n\n", SAMPLE_INTERVAL_MS);

    /* Configure high-priority real-time attributes */
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);

    param.sched_priority = RT_PRIORITY;
    pthread_attr_setschedparam(&attr, &param);

    /* Create the real-time thread */
    rc = pthread_create(&rt_tid, &attr, telemetry_thread, NULL);
    if (rc != 0) {
        fprintf(stderr, "pthread_create failed: %d\n", rc);
        return EXIT_FAILURE;
    }

    pthread_attr_destroy(&attr);

    /* Main thread stays at default priority and waits */
    printf("[MAIN] Created RT thread. Waiting for completion...\n");
    pthread_join(rt_tid, NULL);

    printf("[MAIN] Done. Use 'pidin -p qnx_monitor' while running to verify priority.\n");
    return EXIT_SUCCESS;
}
