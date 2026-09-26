#ifndef GHM_GITHUB_H
#define GHM_GITHUB_H

#include <ghm/ghm.h>
#include <stdint.h>

typedef struct {
    char *device_code;
    char *user_code;
    char *verification_uri;
    unsigned interval;
    unsigned expires_in;
} GhmDeviceCode;

typedef struct {
    char *access_token;
    char *refresh_token;
    int64_t expires_at;
    int64_t refresh_expires_at;
} GhmOAuthToken;

typedef enum {
    GHM_AUTH_PENDING,
    GHM_AUTH_SLOW_DOWN,
    GHM_AUTH_SUCCESS,
    GHM_AUTH_EXPIRED,
    GHM_AUTH_DENIED
} GhmAuthPollStatus;

/* A client ID belongs to an OAuth app with Device Flow enabled. Never pass a client secret. */
int ghm_github_auth_begin(const char *client_id, const char *scope,
                          GhmDeviceCode *out, GhmError *error);
int ghm_github_auth_poll(const char *client_id, const GhmDeviceCode *device,
                         GhmAuthPollStatus *status, GhmOAuthToken *token,
                         unsigned *next_interval, GhmError *error);
int ghm_github_auth_refresh(const char *client_id, const char *refresh_token,
                            GhmOAuthToken *out, GhmError *error);
void ghm_device_code_clear(GhmDeviceCode *device);
void ghm_oauth_token_clear(GhmOAuthToken *token);

/* Tokens live in the desktop secret service, never in SQLite. */
int ghm_credentials_store(const char *client_id, const GhmOAuthToken *token, GhmError *error);
int ghm_credentials_load(const char *client_id, GhmOAuthToken *out, GhmError *error);
int ghm_credentials_clear(const char *client_id, GhmError *error);
/* Loads and, when necessary, refreshes the saved OAuth session. Caller frees
 * the returned token with ghm_access_token_free(), never logs it. */
int ghm_credentials_access_token(const char *client_id, char **out_token, GhmError *error);
void ghm_access_token_free(char *token);

/* Returns the authenticated GitHub login. Caller frees with free(). */
int ghm_github_current_user(const char *access_token, char **login, GhmError *error);

#endif
