cmake_minimum_required(VERSION 3.21)
project(ghm VERSION 0.1.0 LANGUAGES C)
include(GNUInstallDirs)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(GHM_GITHUB_CLIENT_ID "" CACHE STRING "Public GitHub OAuth app Client ID for device login")
if(GHM_GITHUB_CLIENT_ID AND NOT GHM_GITHUB_CLIENT_ID MATCHES "^[A-Za-z0-9]+$")
    message(FATAL_ERROR "GHM_GITHUB_CLIENT_ID must contain only ASCII letters and digits")
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4>=4.12)
pkg_check_modules(LIBGIT2 REQUIRED IMPORTED_TARGET libgit2)
pkg_check_modules(LIBCURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SQLITE3 REQUIRED IMPORTED_TARGET sqlite3)
pkg_check_modules(LIBSECRET REQUIRED IMPORTED_TARGET libsecret-1)
pkg_check_modules(JSON_GLIB REQUIRED IMPORTED_TARGET json-glib-1.0)
find_package(Threads REQUIRED)

set(GHM_CORE_SOURCES
    src/core/branch.c
    src/core/commit.c
    src/core/datetime.c
    src/core/diff.c
    src/core/files.c
    src/core/file_ops.c
    src/core/git.c
    src/core/history.c
    src/core/index_lock.c
    src/core/log.c
    src/core/lock.c
    src/core/mutation.c
    src/core/repository.c
    src/core/remote.c
    src/core/rewrite.c
    src/core/staging.c
    src/core/storage_check.c
    src/github/github_api.c
    src/github/github_auth.c
    src/github/github_repos.c
    src/security/credentials.c
    src/scheduler/scheduler.c
    src/storage/database.c
    src/storage/migrations.c
)
add_library(ghmcore STATIC ${GHM_CORE_SOURCES})
target_include_directories(ghmcore PUBLIC include PRIVATE src)
target_link_libraries(ghmcore PUBLIC PkgConfig::LIBGIT2 PkgConfig::LIBCURL
    PkgConfig::SQLITE3 PkgConfig::LIBSECRET PkgConfig::JSON_GLIB Threads::Threads)
target_compile_definitions(ghmcore PRIVATE _POSIX_C_SOURCE=200809L)

set(GHM_GUI_SOURCES
    src/gui/branch_view.c
    src/gui/commit_view.c
    src/gui/file_view.c
    src/gui/github_repos_view.c
    src/gui/history_view.c
    src/gui/login_view.c
    src/gui/scheduler_view.c
    src/gui/settings_view.c
    src/gui/window.c
)
add_executable(ghm-gui src/gui/application.c ${GHM_GUI_SOURCES})
target_include_directories(ghm-gui PRIVATE include src)
target_link_libraries(ghm-gui PRIVATE ghmcore PkgConfig::GTK4)
target_compile_definitions(ghm-gui PRIVATE _POSIX_C_SOURCE=200809L)
if(GHM_GITHUB_CLIENT_ID)
    target_compile_definitions(ghm-gui PRIVATE GHM_GITHUB_CLIENT_ID=\"${GHM_GITHUB_CLIENT_ID}\")
endif()

add_executable(ghm-worker src/scheduler/worker.c)
target_include_directories(ghm-worker PRIVATE include)
target_link_libraries(ghm-worker PRIVATE ghmcore)
target_compile_definitions(ghm-worker PRIVATE _POSIX_C_SOURCE=200809L)

add_executable(ghm src/cli/main.c)
target_include_directories(ghm PRIVATE include)
target_link_libraries(ghm PRIVATE ghmcore)
target_compile_definitions(ghm PRIVATE _POSIX_C_SOURCE=200809L)
if(GHM_GITHUB_CLIENT_ID)
    target_compile_definitions(ghm PRIVATE GHM_GITHUB_CLIENT_ID=\"${GHM_GITHUB_CLIENT_ID}\")
endif()

foreach(target ghmcore ghm-gui ghm-worker ghm)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
endforeach()

include(CTest)
if(BUILD_TESTING)
    # Fault injection is compiled only into this uninstalled test library.
    add_library(ghmcore-fault STATIC ${GHM_CORE_SOURCES} tests/fault_hooks.c)
    target_include_directories(ghmcore-fault PUBLIC include PRIVATE src tests)
    target_link_libraries(ghmcore-fault PUBLIC PkgConfig::LIBGIT2 PkgConfig::LIBCURL
        PkgConfig::SQLITE3 PkgConfig::LIBSECRET PkgConfig::JSON_GLIB Threads::Threads)
    target_compile_definitions(ghmcore-fault PRIVATE _POSIX_C_SOURCE=200809L GHM_FAULT_TESTING)
    target_compile_options(ghmcore-fault PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    foreach(test_name worktree failure retry)
        add_executable(ghm-${test_name}-test tests/${test_name}_test.c)
        target_include_directories(ghm-${test_name}-test PRIVATE include src tests)
        target_link_libraries(ghm-${test_name}-test PRIVATE ghmcore-fault)
        target_compile_definitions(ghm-${test_name}-test PRIVATE _POSIX_C_SOURCE=200809L)
        target_compile_options(ghm-${test_name}-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        add_test(NAME ghm-${test_name} COMMAND ghm-${test_name}-test)
    endforeach()
    add_executable(ghm-full-disk-test tests/full_disk_test.c)
    target_include_directories(ghm-full-disk-test PRIVATE include src tests)
    target_link_libraries(ghm-full-disk-test PRIVATE ghmcore-fault)
    target_compile_definitions(ghm-full-disk-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-full-disk-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_executable(ghm-gui-workflow tests/gui_workflow.c ${GHM_GUI_SOURCES})
    target_include_directories(ghm-gui-workflow PRIVATE include src tests)
    target_link_libraries(ghm-gui-workflow PRIVATE ghmcore-fault PkgConfig::GTK4)
    target_compile_definitions(ghm-gui-workflow PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-gui-workflow PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    # Manual display-dependent smoke harness; never run automatically on user state.
    add_executable(ghm-gui-smoke tests/gui_smoke.c ${GHM_GUI_SOURCES})
    target_include_directories(ghm-gui-smoke PRIVATE include src)
    target_link_libraries(ghm-gui-smoke PRIVATE ghmcore PkgConfig::GTK4)
    target_compile_definitions(ghm-gui-smoke PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-gui-smoke PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_executable(ghm-core-test tests/core_test.c)
    target_include_directories(ghm-core-test PRIVATE include)
    target_link_libraries(ghm-core-test PRIVATE ghmcore)
    target_compile_definitions(ghm-core-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-core-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-core COMMAND ghm-core-test)
    add_executable(ghm-auth-test tests/auth_test.c)
    target_include_directories(ghm-auth-test PRIVATE include src)
    target_link_libraries(ghm-auth-test PRIVATE ghmcore)
    target_compile_options(ghm-auth-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-auth COMMAND ghm-auth-test)
    add_executable(ghm-snapshot-test tests/snapshot_test.c)
    target_include_directories(ghm-snapshot-test PRIVATE include)
    target_link_libraries(ghm-snapshot-test PRIVATE ghmcore)
    target_compile_definitions(ghm-snapshot-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-snapshot-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-snapshot COMMAND ghm-snapshot-test)
    add_executable(ghm-scheduler-test tests/scheduler_test.c)
    target_include_directories(ghm-scheduler-test PRIVATE include)
    target_link_libraries(ghm-scheduler-test PRIVATE ghmcore)
    target_compile_definitions(ghm-scheduler-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-scheduler-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-scheduler COMMAND ghm-scheduler-test)
    add_executable(ghm-worker-test tests/worker_test.c)
    target_include_directories(ghm-worker-test PRIVATE include)
    target_link_libraries(ghm-worker-test PRIVATE ghmcore)
    target_compile_definitions(ghm-worker-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-worker-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-worker COMMAND ghm-worker-test $<TARGET_FILE:ghm-worker>)
    add_executable(ghm-worker-retry-test tests/worker_retry_test.c)
    target_include_directories(ghm-worker-retry-test PRIVATE include src tests)
    target_link_libraries(ghm-worker-retry-test PRIVATE ghmcore)
    target_compile_definitions(ghm-worker-retry-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-worker-retry-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-worker-retry COMMAND ghm-worker-retry-test $<TARGET_FILE:ghm-worker>)
    add_executable(ghm-github-repos-test tests/github_repos_test.c)
    target_include_directories(ghm-github-repos-test PRIVATE include src)
    target_link_libraries(ghm-github-repos-test PRIVATE ghmcore)
    target_compile_definitions(ghm-github-repos-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-github-repos-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-github-repos COMMAND ghm-github-repos-test)
    add_executable(ghm-commit-now-test tests/commit_now_test.c)
    target_include_directories(ghm-commit-now-test PRIVATE include)
    target_link_libraries(ghm-commit-now-test PRIVATE ghmcore)
    target_compile_definitions(ghm-commit-now-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-commit-now-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-commit-now COMMAND ghm-commit-now-test)
    add_executable(ghm-remote-push-test tests/remote_push_test.c)
    target_include_directories(ghm-remote-push-test PRIVATE include src)
    target_link_libraries(ghm-remote-push-test PRIVATE ghmcore)
    target_compile_definitions(ghm-remote-push-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-remote-push-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-remote-push COMMAND ghm-remote-push-test)
    add_executable(ghm-remote-sync-test tests/remote_sync_test.c)
    target_include_directories(ghm-remote-sync-test PRIVATE include src)
    target_link_libraries(ghm-remote-sync-test PRIVATE ghmcore)
    target_compile_definitions(ghm-remote-sync-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-remote-sync-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-remote-sync COMMAND ghm-remote-sync-test)
    add_executable(ghm-branch-history-test tests/branch_history_test.c)
    target_include_directories(ghm-branch-history-test PRIVATE include)
    target_link_libraries(ghm-branch-history-test PRIVATE ghmcore)
    target_compile_definitions(ghm-branch-history-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-branch-history-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-branch-history COMMAND ghm-branch-history-test)
    add_executable(ghm-merge-test tests/merge_test.c)
    target_include_directories(ghm-merge-test PRIVATE include)
    target_link_libraries(ghm-merge-test PRIVATE ghmcore)
    target_compile_definitions(ghm-merge-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-merge-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-merge COMMAND ghm-merge-test)
    add_executable(ghm-job-guard-test tests/job_guard_test.c)
    target_include_directories(ghm-job-guard-test PRIVATE include src)
    target_link_libraries(ghm-job-guard-test PRIVATE ghmcore)
    target_compile_definitions(ghm-job-guard-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-job-guard-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-job-guard COMMAND ghm-job-guard-test)
    add_executable(ghm-lock-test tests/lock_test.c)
    target_include_directories(ghm-lock-test PRIVATE include src)
    target_link_libraries(ghm-lock-test PRIVATE ghmcore)
    target_compile_definitions(ghm-lock-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-lock-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-lock COMMAND ghm-lock-test)
    add_executable(ghm-datetime-test tests/datetime_test.c)
    target_include_directories(ghm-datetime-test PRIVATE include)
    target_link_libraries(ghm-datetime-test PRIVATE ghmcore)
    target_compile_options(ghm-datetime-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-datetime COMMAND ghm-datetime-test)
    add_executable(ghm-log-test tests/log_test.c)
    target_include_directories(ghm-log-test PRIVATE include)
    target_link_libraries(ghm-log-test PRIVATE ghmcore)
    target_compile_definitions(ghm-log-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-log-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-log COMMAND ghm-log-test)
    add_executable(ghm-diff-test tests/diff_test.c)
    target_include_directories(ghm-diff-test PRIVATE include)
    target_link_libraries(ghm-diff-test PRIVATE ghmcore)
    target_compile_definitions(ghm-diff-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-diff-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-diff COMMAND ghm-diff-test)
    add_executable(ghm-staging-test tests/staging_test.c)
    target_include_directories(ghm-staging-test PRIVATE include)
    target_link_libraries(ghm-staging-test PRIVATE ghmcore)
    target_compile_definitions(ghm-staging-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-staging-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-staging COMMAND ghm-staging-test)
    add_executable(ghm-file-ops-test tests/file_ops_test.c)
    target_include_directories(ghm-file-ops-test PRIVATE include)
    target_link_libraries(ghm-file-ops-test PRIVATE ghmcore)
    target_compile_definitions(ghm-file-ops-test PRIVATE _POSIX_C_SOURCE=200809L)
    target_compile_options(ghm-file-ops-test PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
    add_test(NAME ghm-file-ops COMMAND ghm-file-ops-test)
endif()

configure_file(data/io.github.ghm.CommitManager.desktop.in
    ${CMAKE_CURRENT_BINARY_DIR}/io.github.ghm.CommitManager.desktop @ONLY)
configure_file(data/ghm-worker.service.in
    ${CMAKE_CURRENT_BINARY_DIR}/ghm-worker.service @ONLY)

install(TARGETS ghm-gui ghm-worker ghm RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/io.github.ghm.CommitManager.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ghm-worker.service
    DESTINATION ${CMAKE_INSTALL_DATADIR}/systemd/user)
install(FILES data/io.github.ghm.CommitManager.svg
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)
