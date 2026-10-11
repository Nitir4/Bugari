#include <ghm/scheduler.h>
#include "platform/io.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static HANDLE stopping;
static BOOL WINAPI stop_worker(DWORD reason)
{
    if (reason == CTRL_C_EVENT || reason == CTRL_BREAK_EVENT ||
        reason == CTRL_CLOSE_EVENT || reason == CTRL_LOGOFF_EVENT || reason == CTRL_SHUTDOWN_EVENT) {
        SetEvent(stopping); return TRUE;
    }
    return FALSE;
}

int main(void)
{
    GhmContext *context = NULL;
    GhmError error = {0};
    int lock = -1, result = EXIT_FAILURE;
    HANDLE timer = NULL;
    stopping = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (stopping == NULL || !SetConsoleCtrlHandler(stop_worker, TRUE)) goto done;
    if (ghm_context_open(NULL, &context, &error) != 0) goto done;
    char *path = g_build_filename(ghm_context_data_directory(context), "worker.lock", NULL);
    lock = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    g_free(path);
    if (lock < 0 || ghm_platform_lock(lock) != 0) {
        fprintf(stderr, "ghm-worker: another worker is running or its lock is unavailable\n"); goto done;
    }
    timer = CreateWaitableTimerW(NULL, FALSE, NULL);
    LARGE_INTEGER first = {.QuadPart = -10000000LL};
    if (timer == NULL || !SetWaitableTimer(timer, &first, 1000, NULL, NULL, FALSE)) goto done;
    if (ghm_schedule_recover(context, (int64_t)time(NULL), &error) != 0) goto done;
    int recovery_pending = error.code == GHM_ERROR_BUSY;
    if (ghm_schedule_network_changed(context, &error) != 0) goto done;
    HANDLE events[] = {stopping, timer};
    for (;;) {
        if (WaitForSingleObject(stopping, 0) == WAIT_OBJECT_0) break;
        if (recovery_pending) {
            error = (GhmError){0};
            if (ghm_schedule_recover(context, (int64_t)time(NULL), &error) != 0) goto done;
            recovery_pending = error.code == GHM_ERROR_BUSY;
        }
        int executed, pushed;
        do {
            error = (GhmError){0};
            executed = ghm_schedule_run_one_due(context, (int64_t)time(NULL), &error);
            if (executed < 0) goto done;
            error = (GhmError){0};
            pushed = ghm_schedule_run_one_push(context, &error);
            if (pushed < 0) goto done;
        } while ((executed > 0 || pushed > 0) && WaitForSingleObject(stopping, 0) != WAIT_OBJECT_0);
        /* A one-second timer also observes new SQLite jobs and network retry
         * deadlines. No directory-watch race can leave a new job asleep. */
        DWORD event = WaitForMultipleObjects(2, events, FALSE, INFINITE);
        if (event == WAIT_OBJECT_0) break;
        if (event != WAIT_OBJECT_0 + 1) goto done;
    }
    result = EXIT_SUCCESS;
done:
    if (result != EXIT_SUCCESS && error.message[0]) fprintf(stderr, "ghm-worker: %s\n", error.message);
    if (timer != NULL) CloseHandle(timer);
    if (lock >= 0) close(lock);
    ghm_context_close(context);
    if (stopping != NULL) { SetConsoleCtrlHandler(stop_worker, FALSE); CloseHandle(stopping); }
    return result;
}
