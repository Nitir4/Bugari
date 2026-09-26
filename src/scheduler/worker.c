#include <ghm/scheduler.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/inotify.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>

static int watch_network(void)
{
    int descriptor = socket(AF_NETLINK, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_ROUTE);
    struct sockaddr_nl address = {.nl_family = AF_NETLINK,
        .nl_groups = RTMGRP_LINK | RTMGRP_IPV4_IFADDR | RTMGRP_IPV6_IFADDR | RTMGRP_IPV4_ROUTE | RTMGRP_IPV6_ROUTE};
    if (descriptor >= 0 && bind(descriptor, (struct sockaddr *)&address, sizeof(address)) != 0) {
        close(descriptor); descriptor = -1;
    }
    return descriptor; /* Timed retries still work where netlink is unavailable. */
}

static int consume_network(int descriptor)
{
    char bytes[8192];
    int changed = 0;
    for (;;) {
        ssize_t count = recv(descriptor, bytes, sizeof(bytes), MSG_DONTWAIT);
        if (count > 0) { changed = 1; continue; }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
    return changed;
}

static volatile sig_atomic_t stopping = 0;

static void stop_worker(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int drain_notifications(int descriptor)
{
    char buffer[4096];
    for (;;) {
        ssize_t received = read(descriptor, buffer, sizeof(buffer));
        if (received > 0) continue;
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && errno == EAGAIN) return 0;
        return -1;
    }
}

static int arm_timer(int descriptor, int64_t execute_at)
{
    struct itimerspec schedule = {0};
    if (execute_at >= 0) schedule.it_value.tv_sec = (time_t)execute_at;
    return timerfd_settime(descriptor, TFD_TIMER_ABSTIME, &schedule, NULL);
}

int main(void)
{
    GhmContext *context = NULL;
    GhmError error = {0};
    char *lock_name = NULL;
    int lock_fd = -1;
    int inotify_fd = -1;
    int timer_fd = -1;
    int network_fd = -1;
    int result = EXIT_FAILURE;
    const char *directory;
    int retry_seconds = 1;
    int recovery_pending = 0;
    struct sigaction action = {0};
    if (ghm_context_open(NULL, &context, &error) != 0) {
        fprintf(stderr, "ghm-worker: %s\n", error.message);
        return EXIT_FAILURE;
    }
    directory = ghm_context_data_directory(context);
    lock_name = malloc(strlen(directory) + sizeof("/worker.lock"));
    if (lock_name == NULL) goto done;
    (void)snprintf(lock_name, strlen(directory) + sizeof("/worker.lock"),
                   "%s/worker.lock", directory);
    lock_fd = open(lock_name, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        fprintf(stderr, "ghm-worker: another worker is already running or lock unavailable\n");
        goto done;
    }
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    timer_fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
    if (inotify_fd < 0 || timer_fd < 0 ||
        inotify_add_watch(inotify_fd, directory,
                          IN_MODIFY | IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE) < 0) {
        fprintf(stderr, "ghm-worker: cannot watch job database: %s\n", strerror(errno));
        goto done;
    }
    action.sa_handler = stop_worker;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGINT, &action, NULL) != 0 || sigaction(SIGTERM, &action, NULL) != 0) goto done;
    if (ghm_schedule_recover(context, (int64_t)time(NULL), &error) != 0) {
        fprintf(stderr, "ghm-worker: recovery failed: %s\n", error.message);
        goto done;
    }
    recovery_pending = error.code == GHM_ERROR_BUSY;
    network_fd = watch_network();
    if (ghm_schedule_network_changed(context, &error) != 0) goto done;
    while (!stopping) {
        int executed;
        int pushed;
        int64_t next_due = -1;
        int busy = 0;
        struct pollfd wait_for[3] = {
            {.fd = inotify_fd, .events = POLLIN},
            {.fd = timer_fd, .events = POLLIN},
            {.fd = network_fd, .events = POLLIN}
        };
        if (drain_notifications(inotify_fd) != 0) goto done;
        if (recovery_pending) {
            error = (GhmError){0};
            if (ghm_schedule_recover(context, (int64_t)time(NULL), &error) != 0) goto done;
            recovery_pending = error.code == GHM_ERROR_BUSY;
            if (recovery_pending) busy = 1;
        }
        do {
            error = (GhmError){0};
            pushed = ghm_schedule_run_one_push(context, &error);
            if (error.code == GHM_ERROR_BUSY) busy = 1;
            if (pushed < 0) {
                fprintf(stderr, "ghm-worker: scheduled push failed: %s\n", error.message);
                goto done;
            }
            error = (GhmError){0};
            executed = ghm_schedule_run_one_due(context, (int64_t)time(NULL), &error);
            if (error.code == GHM_ERROR_BUSY) busy = 1;
            if (executed < 0) {
                fprintf(stderr, "ghm-worker: job execution failed: %s\n", error.message);
                goto done;
            }
        } while ((executed > 0 || pushed > 0) && !stopping);
        if (stopping) break;
        if (ghm_schedule_next_due(context, &next_due, &error) != 0) goto done;
        if (busy) {
            next_due = (int64_t)time(NULL) + retry_seconds;
            retry_seconds = retry_seconds < 15 ? retry_seconds * 2 : 30;
        } else retry_seconds = 1;
        if (arm_timer(timer_fd, next_due) != 0) {
            fprintf(stderr, "ghm-worker: cannot arm next job: %s\n",
                    error.message[0] != '\0' ? error.message : strerror(errno));
            goto done;
        }
        if (poll(wait_for, 3, -1) < 0 && errno != EINTR) {
            fprintf(stderr, "ghm-worker: wait failed: %s\n", strerror(errno));
            goto done;
        }
        if ((wait_for[2].revents & POLLIN) != 0 && consume_network(network_fd) &&
            ghm_schedule_network_changed(context, &error) != 0) goto done;
        if ((wait_for[1].revents & POLLIN) != 0) {
            uint64_t expirations;
            (void)read(timer_fd, &expirations, sizeof(expirations));
        }
    }
    result = EXIT_SUCCESS;
done:
    if (network_fd >= 0) close(network_fd);
    if (timer_fd >= 0) close(timer_fd);
    if (inotify_fd >= 0) close(inotify_fd);
    if (lock_fd >= 0) close(lock_fd);
    free(lock_name);
    ghm_context_close(context);
    return result;
}
