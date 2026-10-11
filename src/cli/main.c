#include <ghm/branch.h>
#include <ghm/commit.h>
#include <ghm/datetime.h>
#include <ghm/github.h>
#include <ghm/github_repos.h>
#include <ghm/history.h>
#include <ghm/log.h>
#include <ghm/remote.h>
#include <ghm/scheduler.h>

#include <glib.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#include <glib/gwin32.h>
#endif

#ifndef GHM_GITHUB_CLIENT_ID
#define GHM_GITHUB_CLIENT_ID ""
#endif

static void usage(void)
{
    puts("GitHub Commit Manager (native CLI)\n"
         "  ghm login [--public-only]\n"
         "  ghm repo list [--github]\n"
         "  ghm repo clone OWNER/NAME\n"
         "  ghm repo branches OWNER/NAME\n"
         "  ghm repo open [PATH_OR_NAME]\n"
         "  ghm repo status [PATH_OR_NAME]\n"
         "  ghm commit [--message TEXT] [--date ISO8601] [--all]\n"
         "  ghm push | fetch | pull\n"
         "  ghm schedule --message TEXT --at ISO8601 [--date ISO8601] [--push] [--staged]\n"
         "  ghm schedule list [PATH_OR_NAME]\n"
         "  ghm schedule cancel ID\n"
         "  ghm schedule reschedule ID --at ISO8601\n"
         "  ghm schedule edit ID --message TEXT (--push | --no-push)\n"
         "  ghm schedule retry-push ID\n"
         "  ghm history [PATH_OR_NAME]\n"
         "  ghm branch list | create NAME | checkout NAME | merge NAME\n"
         "  ghm rewrite-head --expect FULL_SHA --message TEXT --date ISO8601 --confirm\n"
         "Dates require an explicit offset, e.g. 2026-09-20T14:30:00+05:30.\n"
         "Commands without a repository argument use the current directory's repository.");
}

static char *client_id(GhmContext *context)
{
    const char *environment = getenv("GHM_GITHUB_CLIENT_ID");
    char *saved = NULL;
    GhmError ignored = {0};
    if (environment != NULL && environment[0] != '\0') return strdup(environment);
    if (GHM_GITHUB_CLIENT_ID[0] != '\0') return strdup(GHM_GITHUB_CLIENT_ID);
    if (ghm_setting_get(context, "github_client_id", &saved, &ignored) != 0) return NULL;
    return saved;
}

static char *resolve_repository(GhmContext *context, const char *name, GhmError *error)
{
    GhmRepositoryList repositories = {0};
    char *path = NULL;
    const char *candidate = name != NULL ? name : ".";
    GhmError ignored = {0};
    if (ghm_repo_find(candidate, &path, &ignored) == 0) return path;
    if (ghm_repo_list(context, &repositories, error) != 0) return NULL;
    for (size_t i = 0; i < repositories.count; ++i) {
        if (strcmp(repositories.items[i].name, candidate) == 0 ||
            strcmp(repositories.items[i].path, candidate) == 0) {
            path = strdup(repositories.items[i].path);
            break;
        }
    }
    ghm_repo_list_free(&repositories);
    if (path == NULL) {
        error->code = GHM_ERROR_ARGUMENT;
        (void)snprintf(error->message, sizeof(error->message),
                       "Repository '%s' was not found locally", candidate);
    }
    return path;
}

static int status_command(GhmContext *context, const char *name, int open_only, GhmError *error)
{
    char *path = resolve_repository(context, name, error);
    GhmStatus status = {0};
    int result = -1;
    if (path == NULL) return -1;
    if (open_only) { puts(path); result = 0; goto done; }
    if (ghm_repo_status(path, &status, error) != 0) goto done;
    printf("%s  %s\n", path, status.detached ? "(detached HEAD)" :
           status.branch != NULL ? status.branch : "(unborn branch)");
    for (size_t i = 0; i < status.count; ++i) {
        const GhmStatusEntry *entry = &status.items[i];
        printf("%c%c %-10s %s\n", entry->staged ? 'S' : ' ',
               entry->unstaged ? 'U' : ' ', ghm_status_kind_label(entry->kind), entry->path);
    }
    if (status.count == 0) puts("Working tree clean");
    result = 0;
done:
    ghm_status_free(&status);
    free(path);
    return result;
}

static int repo_command(GhmContext *context, int argc, char **argv, GhmError *error)
{
    if (argc < 1) { usage(); return -1; }
    if (strcmp(argv[0], "list") == 0) {
        if (argc == 2 && strcmp(argv[1], "--github") == 0) {
            GhmGitHubRepoList remote = {0};
            char *id = client_id(context);
            int result = id != NULL ? ghm_github_repos_list(id, &remote, error) : -1;
            if (id == NULL) {
                error->code = GHM_ERROR_AUTH;
                (void)snprintf(error->message, sizeof(error->message), "GitHub OAuth client ID is not configured");
            }
            if (result == 0)
                for (size_t i = 0; i < remote.count; ++i)
                    printf("%s%s%s  stars:%" PRId64 "  forks:%" PRId64 "%s%s\n",
                           remote.items[i].full_name,
                           remote.items[i].is_private ? "  (private)" : "",
                           remote.items[i].is_archived ? "  (archived)" : "",
                           remote.items[i].stargazers_count, remote.items[i].forks_count,
                           remote.items[i].description[0] != '\0' ? "  " : "",
                           remote.items[i].description);
            ghm_github_repos_free(&remote);
            free(id);
            return result;
        }
        if (argc != 1) { usage(); return -1; }
        GhmRepositoryList local = {0};
        if (ghm_repo_discover(context, error) != 0 || ghm_repo_list(context, &local, error) != 0)
            return -1;
        for (size_t i = 0; i < local.count; ++i)
            printf("%s\t%s\n", local.items[i].name, local.items[i].path);
        ghm_repo_list_free(&local);
        return 0;
    }
    if (strcmp(argv[0], "branches") == 0 && argc == 2) {
        GhmGitHubBranchList branches = {0};
        char *id = client_id(context);
        int result = -1;
        if (id == NULL) {
            error->code = GHM_ERROR_AUTH;
            (void)snprintf(error->message, sizeof(error->message),
                           "GitHub OAuth client ID is not configured");
        } else {
            result = ghm_github_branches_list(id, argv[1], &branches, error);
            if (result == 0)
                for (size_t i = 0; i < branches.count; ++i)
                    printf("%.12s  %s%s\n", branches.items[i].head_oid,
                           branches.items[i].name,
                           branches.items[i].is_protected ? "  (protected)" : "");
        }
        ghm_github_branches_free(&branches);
        free(id);
        return result;
    }
    if (strcmp(argv[0], "clone") == 0 && argc == 2) {
        GhmGitHubRepoList remote = {0};
        char *id = client_id(context), *path = NULL;
        int result = -1, found = 0;
        if (id == NULL) {
            error->code = GHM_ERROR_AUTH;
            (void)snprintf(error->message, sizeof(error->message), "GitHub OAuth client ID is not configured");
            return -1;
        }
        if (ghm_github_repos_list(id, &remote, error) != 0) goto clone_done;
        for (size_t i = 0; i < remote.count; ++i) {
            if (strcmp(remote.items[i].full_name, argv[1]) != 0) continue;
            found = 1;
            result = ghm_repo_clone_github(context, id, &remote.items[i], &path, error);
            if (result == 0) puts(path);
            break;
        }
        if (!found) {
            error->code = GHM_ERROR_ARGUMENT;
            (void)snprintf(error->message, sizeof(error->message), "Repository not visible to this GitHub account");
        }
clone_done:
        free(path);
        ghm_github_repos_free(&remote);
        free(id);
        return result;
    }
    if (strcmp(argv[0], "status") == 0 && argc <= 2)
        return status_command(context, argc == 2 ? argv[1] : NULL, 0, error);
    if (strcmp(argv[0], "open") == 0 && argc <= 2)
        return status_command(context, argc == 2 ? argv[1] : NULL, 1, error);
    usage();
    return -1;
}

static int default_signature(const char *path, GhmCommitSignature *signature,
                             char **name, char **email, GhmError *error)
{
    GDateTime *now;
    if (ghm_commit_default_identity(path, name, email, error) != 0) return -1;
    now = g_date_time_new_now_local();
    signature->name = *name;
    signature->email = *email;
    signature->timestamp = g_date_time_to_unix(now);
    signature->offset_minutes = (int)(g_date_time_get_utc_offset(now) / G_TIME_SPAN_MINUTE);
    g_date_time_unref(now);
    return 0;
}

static int commit_command(GhmContext *context, int argc, char **argv, GhmError *error)
{
    char *path = resolve_repository(context, NULL, error);
    char *name = NULL, *email = NULL;
    const char *message = NULL, *date = NULL;
    char prompt[4096], oid[GHM_OID_HEX_CAPACITY] = {0};
    GhmCommitSignature signature = {0};
    int all = 0, result = -1;
    if (path == NULL) return -1;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) message = argv[++i];
        else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) date = argv[++i];
        else if (strcmp(argv[i], "--all") == 0) all = 1;
        else { usage(); goto done; }
    }
    if (message == NULL) {
        fputs("Commit message: ", stdout);
        fflush(stdout);
        if (fgets(prompt, sizeof(prompt), stdin) == NULL) {
            error->code = GHM_ERROR_ARGUMENT;
            (void)snprintf(error->message, sizeof(error->message), "Commit message is required");
            goto done;
        }
        prompt[strcspn(prompt, "\r\n")] = '\0';
        message = prompt;
    }
    if (default_signature(path, &signature, &name, &email, error) != 0) goto done;
    if (date != NULL && ghm_datetime_parse(date, &signature.timestamp,
                                           &signature.offset_minutes, error) != 0) goto done;
    if ((all ? ghm_commit_now(path, message, &signature, &signature, oid, error) :
               ghm_commit_staged(path, message, &signature, &signature, oid, error)) != 0) goto done;
    printf("Created commit %s\n", oid);
    result = 0;
done:
    free(name);
    free(email);
    free(path);
    return result;
}

static int sync_command(GhmContext *context, const char *command, GhmError *error)
{
    char *path = resolve_repository(context, NULL, error);
    char *id = client_id(context);
    char oid[GHM_OID_HEX_CAPACITY] = {0};
    GhmPullOutcome outcome = GHM_PULL_UP_TO_DATE;
    int result = -1;
    if (path == NULL) goto done;
    if (strcmp(command, "push") == 0) {
        result = ghm_repo_push(path, id, oid, error);
        if (result == 0) printf("Pushed %s\n", oid);
    } else if (strcmp(command, "fetch") == 0) {
        result = ghm_repo_fetch(path, id, error);
        if (result == 0) puts("Fetched origin; local worktree unchanged");
    } else {
        result = ghm_repo_pull(context, path, id, &outcome, error);
        if (result == 0) puts(outcome == GHM_PULL_FAST_FORWARDED ? "Fast-forwarded" :
                              outcome == GHM_PULL_LOCAL_AHEAD ? "Local branch is ahead" : "Already up to date");
    }
done:
    free(path);
    free(id);
    return result;
}

static int parse_job_id(const char *text, int64_t *out, GhmError *error)
{
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value <= 0) {
        error->code = GHM_ERROR_ARGUMENT;
        (void)snprintf(error->message, sizeof(error->message), "Job ID must be a positive number");
        return -1;
    }
    *out = (int64_t)value;
    return 0;
}

static int schedule_command(GhmContext *context, int argc, char **argv, GhmError *error)
{
    char *path = NULL, *name = NULL, *email = NULL, *id = NULL;
    GhmCommitSignature signature = {0};
    int64_t job_id = 0, execute_at = 0;
    int execute_offset = 0, stage_all = 1, push = 0, result = -1;
    const char *message = NULL, *at = NULL, *date = NULL;
    if (argc > 0 && strcmp(argv[0], "cancel") == 0 && argc == 2) {
        if (parse_job_id(argv[1], &job_id, error) != 0) return -1;
        result = ghm_schedule_cancel(context, job_id, error);
        if (result == 0) printf("Cancelled job #%" PRId64 "\n", job_id);
        return result;
    }
    if (argc > 0 && strcmp(argv[0], "reschedule") == 0 && argc == 4 &&
        strcmp(argv[2], "--at") == 0) {
        if (parse_job_id(argv[1], &job_id, error) != 0 ||
            ghm_datetime_parse(argv[3], &execute_at, &execute_offset, error) != 0) return -1;
        result = ghm_schedule_reschedule(context, job_id, execute_at, error);
        if (result == 0) printf("Rescheduled job #%" PRId64 "\n", job_id);
        return result;
    }
    if (argc > 0 && strcmp(argv[0], "edit") == 0 && argc == 5 &&
        strcmp(argv[2], "--message") == 0 &&
        (strcmp(argv[4], "--push") == 0 || strcmp(argv[4], "--no-push") == 0)) {
        if (parse_job_id(argv[1], &job_id, error) != 0) return -1;
        push = strcmp(argv[4], "--push") == 0;
        if (push) {
            id = client_id(context);
            if (id != NULL && ghm_setting_set(context, "github_client_id", id, error) != 0) {
                free(id);
                return -1;
            }
            free(id);
        }
        result = ghm_schedule_edit(context, job_id, argv[3], push, error);
        if (result == 0) printf("Edited job #%" PRId64 "\n", job_id);
        return result;
    }
    if (argc > 0 && strcmp(argv[0], "retry-push") == 0 && argc == 2) {
        if (parse_job_id(argv[1], &job_id, error) != 0) return -1;
        result = ghm_schedule_retry_push(context, job_id, error);
        if (result == 0) printf("Queued push for job #%" PRId64 "\n", job_id);
        return result;
    }
    if (argc > 0 && strcmp(argv[0], "list") == 0 && argc <= 2) {
        GhmScheduledJobList jobs = {0};
        path = resolve_repository(context, argc == 2 ? argv[1] : NULL, error);
        if (path == NULL) return -1;
        result = ghm_schedule_list(context, path, &jobs, error);
        if (result == 0)
            for (size_t i = 0; i < jobs.count; ++i)
                printf("#%" PRId64 "  %s  at %" PRId64 "  %s  push:%s%s%s\n",
                       jobs.items[i].id, jobs.items[i].status, jobs.items[i].execute_at,
                       jobs.items[i].message, jobs.items[i].push_status,
                       jobs.items[i].error_message != NULL ? "  error:" : "",
                       jobs.items[i].error_message != NULL ? jobs.items[i].error_message : "");
        ghm_schedule_list_free(&jobs);
        free(path);
        return result;
    }
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) message = argv[++i];
        else if (strcmp(argv[i], "--at") == 0 && i + 1 < argc) at = argv[++i];
        else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) date = argv[++i];
        else if (strcmp(argv[i], "--push") == 0) push = 1;
        else if (strcmp(argv[i], "--staged") == 0) stage_all = 0;
        else { usage(); return -1; }
    }
    if (message == NULL || at == NULL) { usage(); return -1; }
    path = resolve_repository(context, NULL, error);
    if (path == NULL) goto done;
    if (ghm_datetime_parse(at, &execute_at, &execute_offset, error) != 0 ||
        default_signature(path, &signature, &name, &email, error) != 0) goto done;
    if (date != NULL) {
        if (ghm_datetime_parse(date, &signature.timestamp, &signature.offset_minutes, error) != 0)
            goto done;
    } else {
        signature.timestamp = execute_at;
        signature.offset_minutes = execute_offset;
    }
    if (push) {
        id = client_id(context);
        if (id != NULL && ghm_setting_set(context, "github_client_id", id, error) != 0) goto done;
    }
    result = ghm_schedule_add_options(context, path, message, &signature, &signature,
                                      execute_at, stage_all, push, &job_id, error);
    if (result == 0) printf("Scheduled job #%" PRId64 "\n", job_id);
done:
    free(id);
    free(name);
    free(email);
    free(path);
    return result;
}

static int history_command(GhmContext *context, int argc, char **argv, GhmError *error)
{
    GhmHistory history = {0};
    char *path;
    int result;
    if (argc > 1) { usage(); return -1; }
    path = resolve_repository(context, argc == 1 ? argv[0] : NULL, error);
    if (path == NULL) return -1;
    result = ghm_commit_get_history(path, 100, &history, error);
    if (result == 0)
        for (size_t i = 0; i < history.count; ++i)
            printf("%.12s  %" PRId64 "  %s  %s\n", history.items[i].oid,
                   history.items[i].author_timestamp, history.items[i].author_name,
                   history.items[i].summary);
    ghm_history_free(&history);
    free(path);
    return result;
}

static int branch_command(GhmContext *context, int argc, char **argv, GhmError *error)
{
    char *path = resolve_repository(context, NULL, error);
    int result = -1;
    if (path == NULL) return -1;
    if (argc == 1 && strcmp(argv[0], "list") == 0) {
        GhmBranchList branches = {0};
        result = ghm_branch_list(path, &branches, error);
        if (result == 0)
            for (size_t i = 0; i < branches.count; ++i)
                printf("%c %s%s\n", branches.items[i].is_current ? '*' : ' ',
                       branches.items[i].is_remote ? "remote " : "",
                       branches.items[i].name);
        ghm_branch_list_free(&branches);
    } else if (argc == 2 && strcmp(argv[0], "create") == 0) {
        result = ghm_branch_create(path, argv[1], error);
        if (result == 0) printf("Created branch %s\n", argv[1]);
    } else if (argc == 2 && strcmp(argv[0], "checkout") == 0) {
        result = ghm_branch_checkout(context, path, argv[1], error);
        if (result == 0) printf("Checked out %s\n", argv[1]);
    } else if (argc == 2 && strcmp(argv[0], "merge") == 0) {
        GhmMergeOutcome outcome = GHM_MERGE_UP_TO_DATE;
        char oid[GHM_OID_HEX_CAPACITY] = {0};
        result = ghm_branch_merge(context, path, argv[1], &outcome, oid, error);
        if (result == 0)
            printf("%s: %s\n", outcome == GHM_MERGE_COMMITTED ? "Merge commit" :
                   outcome == GHM_MERGE_FAST_FORWARDED ? "Fast-forwarded" : "Already up to date", oid);
    } else usage();
    free(path);
    return result;
}

static int rewrite_command(GhmContext *context, int argc, char **argv, GhmError *error)
{
    const char *expected = NULL, *message = NULL, *date = NULL;
    char *path = NULL;
    char oid[GHM_OID_HEX_CAPACITY] = {0};
    int64_t timestamp = 0;
    int offset = 0, confirm = 0, result = -1;
    for (int i = 0; i < argc; ++i) {
        if (strcmp(argv[i], "--expect") == 0 && i + 1 < argc) expected = argv[++i];
        else if (strcmp(argv[i], "--message") == 0 && i + 1 < argc) message = argv[++i];
        else if (strcmp(argv[i], "--date") == 0 && i + 1 < argc) date = argv[++i];
        else if (strcmp(argv[i], "--confirm") == 0) confirm = 1;
        else { usage(); return -1; }
    }
    if (expected == NULL || message == NULL || date == NULL || !confirm) { usage(); return -1; }
    path = resolve_repository(context, NULL, error);
    if (path == NULL || ghm_datetime_parse(date, &timestamp, &offset, error) != 0) goto done;
    result = ghm_commit_rewrite_head(context, path, expected, message, timestamp, offset,
                                     timestamp, offset, confirm, oid, error);
    if (result == 0) printf("Rewrote local HEAD: %s\nNormal push will not force-update a remote.\n", oid);
done:
    free(path);
    return result;
}

static int login_command(GhmContext *context, int argc, char **argv, GhmError *error)
{
    char *id = client_id(context), *login = NULL;
    GhmDeviceCode device = {0};
    GhmOAuthToken token = {0};
    GhmAuthPollStatus status = GHM_AUTH_PENDING;
    unsigned interval = 0;
    int result = -1;
    const char *scope = argc == 1 && strcmp(argv[0], "--public-only") == 0 ?
                        "public_repo read:user" : "repo read:user";
    if (argc > 1 || (argc == 1 && strcmp(argv[0], "--public-only") != 0)) {
        usage();
        goto done;
    }
    if (id == NULL) {
        error->code = GHM_ERROR_AUTH;
        (void)snprintf(error->message, sizeof(error->message), "GitHub OAuth client ID is not configured");
        goto done;
    }
    if (ghm_github_auth_begin(id, scope, &device, error) != 0) goto done;
    printf("Open %s and enter code %s\n", device.verification_uri, device.user_code);
    fflush(stdout);
    time_t deadline = time(NULL) + (time_t)device.expires_in;
    interval = device.interval;
    while (time(NULL) < deadline) {
        g_usleep((gulong)(interval < 5 ? 5 : interval) * G_USEC_PER_SEC);
        if (ghm_github_auth_poll(id, &device, &status, &token, &interval, error) != 0) goto done;
        if (status == GHM_AUTH_SUCCESS) break;
        if (status == GHM_AUTH_DENIED || status == GHM_AUTH_EXPIRED) {
            error->code = GHM_ERROR_AUTH;
            (void)snprintf(error->message, sizeof(error->message), "GitHub authorization was denied or expired");
            goto done;
        }
    }
    if (status != GHM_AUTH_SUCCESS) {
        error->code = GHM_ERROR_AUTH;
        (void)snprintf(error->message, sizeof(error->message), "GitHub device code expired");
        goto done;
    }
    if (ghm_credentials_store(id, &token, error) != 0 ||
        ghm_setting_set(context, "github_client_id", id, error) != 0) goto done;
    if (ghm_github_current_user(token.access_token, &login, error) == 0)
        printf("Signed in as %s\n", login);
    else puts("Signed in; profile is temporarily unavailable");
    result = 0;
done:
    free(login);
    ghm_oauth_token_clear(&token);
    ghm_device_code_clear(&device);
    free(id);
    return result;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    g_auto(GStrv) utf8_argv = g_win32_get_command_line();
    argc = (int)g_strv_length(utf8_argv); argv = utf8_argv;
#endif
    GhmContext *context = NULL;
    GhmError error = {0};
    int result = -1;
    if (argc < 2 || strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "help") == 0) {
        usage();
        return argc < 2 ? EXIT_FAILURE : EXIT_SUCCESS;
    }
    if (ghm_context_open(NULL, &context, &error) != 0) goto done;
    if (strcmp(argv[1], "login") == 0) result = login_command(context, argc - 2, argv + 2, &error);
    else if (strcmp(argv[1], "repo") == 0) result = repo_command(context, argc - 2, argv + 2, &error);
    else if (strcmp(argv[1], "commit") == 0) result = commit_command(context, argc - 2, argv + 2, &error);
    else if (strcmp(argv[1], "push") == 0 || strcmp(argv[1], "fetch") == 0 ||
             strcmp(argv[1], "pull") == 0)
        result = argc == 2 ? sync_command(context, argv[1], &error) : -1;
    else if (strcmp(argv[1], "schedule") == 0)
        result = schedule_command(context, argc - 2, argv + 2, &error);
    else if (strcmp(argv[1], "history") == 0)
        result = history_command(context, argc - 2, argv + 2, &error);
    else if (strcmp(argv[1], "branch") == 0)
        result = branch_command(context, argc - 2, argv + 2, &error);
    else if (strcmp(argv[1], "rewrite-head") == 0)
        result = rewrite_command(context, argc - 2, argv + 2, &error);
    else usage();
done:
    if (result != 0 && error.message[0] != '\0' && argc > 1)
        ghm_log_event(GHM_LOG_ERROR, "cli", argv[1], error.message);
    if (result != 0 && error.message[0] != '\0') fprintf(stderr, "ghm: %s\n", error.message);
    ghm_context_close(context);
    return result == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
