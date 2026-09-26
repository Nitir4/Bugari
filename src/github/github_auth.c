#include <ghm/github.h>
#include "core/git.h"
#include "github/github_api.h"
#include "github/github_auth.h"

#include <glib.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void erase_free(char **value)
{
    if (value == NULL || *value == NULL) return;
    volatile char *bytes = (volatile char *)*value;
    size_t length = strlen(*value);
    for (size_t i = 0; i < length; ++i) bytes[i] = '\0';
    free(*value);
    *value = NULL;
}

void ghm_device_code_clear(GhmDeviceCode *device)
{
    if (device == NULL) return;
    erase_free(&device->device_code);
    free(device->user_code);
    free(device->verification_uri);
    *device = (GhmDeviceCode){0};
}

void ghm_oauth_token_clear(GhmOAuthToken *token)
{
    if (token == NULL) return;
    erase_free(&token->access_token);
    erase_free(&token->refresh_token);
    *token = (GhmOAuthToken){0};
}

static int valid_client_id(const char *client_id)
{
    size_t length;
    if (client_id == NULL) return 0;
    length = strlen(client_id);
    if (length == 0 || length > 128) return 0;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = (unsigned char)client_id[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9'))) return 0;
    }
    return 1;
}

static GHashTable *parse_form(const GhmHttpResponse *response, GhmError *error)
{
    GError *parse_error = NULL;
    GHashTable *values = g_uri_parse_params(response->body, -1, "&", G_URI_PARAMS_WWW_FORM, &parse_error);
    if (values == NULL) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "Invalid GitHub OAuth response");
        g_clear_error(&parse_error);
    }
    return values;
}

static const char *field(GHashTable *values, const char *key)
{
    return g_hash_table_lookup(values, key);
}

static unsigned positive_seconds(const char *value, unsigned fallback)
{
    char *end = NULL;
    unsigned long parsed;
    if (value == NULL) return fallback;
    parsed = strtoul(value, &end, 10);
    if (end == value || *end != '\0' || parsed == 0 || parsed > UINT_MAX) return fallback;
    return (unsigned)parsed;
}

static void oauth_error(GHashTable *values, GhmError *error)
{
    const char *code = field(values, "error");
    const char *description = field(values, "error_description");
    if (code != NULL && strcmp(code, "device_flow_disabled") == 0)
        ghm_error_set(error, GHM_ERROR_AUTH, "Enable Device Flow in the GitHub OAuth app settings");
    else if (code != NULL && strcmp(code, "incorrect_client_credentials") == 0)
        ghm_error_set(error, GHM_ERROR_AUTH, "GitHub OAuth client ID is incorrect");
    else if (code != NULL && strcmp(code, "bad_refresh_token") == 0)
        ghm_error_set(error, GHM_ERROR_AUTH, "GitHub session expired; sign in again");
    else if (description != NULL && description[0] != '\0')
        ghm_error_set(error, GHM_ERROR_AUTH, description);
    else
        ghm_error_set(error, GHM_ERROR_AUTH, "GitHub authorization failed");
}

static int token_from_form(GHashTable *values, GhmOAuthToken *token, GhmError *error)
{
    const char *access = field(values, "access_token");
    const char *refresh = field(values, "refresh_token");
    unsigned expires = positive_seconds(field(values, "expires_in"), 0);
    unsigned refresh_expires = positive_seconds(field(values, "refresh_token_expires_in"), 0);
    time_t now = time(NULL);
    if (access == NULL || access[0] == '\0') {
        oauth_error(values, error);
        return -1;
    }
    token->access_token = strdup(access);
    token->refresh_token = refresh != NULL && refresh[0] != '\0' ? strdup(refresh) : NULL;
    if (token->access_token == NULL || (refresh != NULL && refresh[0] != '\0' && token->refresh_token == NULL)) {
        ghm_oauth_token_clear(token);
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    token->expires_at = expires == 0 ? 0 : (int64_t)now + (int64_t)expires;
    token->refresh_expires_at = refresh_expires == 0 ? 0 : (int64_t)now + (int64_t)refresh_expires;
    return 0;
}

int ghm_github_auth_begin(const char *client_id, const char *scope,
                          GhmDeviceCode *out, GhmError *error)
{
    GhmHttpResponse response = {0};
    char *escaped_scope = NULL;
    char *body = NULL;
    int result = -1;
    if (!valid_client_id(client_id) || scope == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "A GitHub OAuth client ID and scope are required");
        return -1;
    }
    *out = (GhmDeviceCode){0};
    escaped_scope = g_uri_escape_string(scope, NULL, TRUE);
    body = g_strdup_printf("client_id=%s&scope=%s", client_id, escaped_scope);
    if (ghm_http_request("https://github.com/login/device/code", body,
                         "application/x-www-form-urlencoded", NULL, &response, error) != 0) goto done;
    result = ghm_auth_parse_device_response(response.body, response.status, out, error);
done:
    ghm_http_response_clear(&response);
    g_free(body);
    g_free(escaped_scope);
    return result;
}

int ghm_auth_parse_device_response(const char *body, long http_status,
                                   GhmDeviceCode *out, GhmError *error)
{
    GhmHttpResponse response = {.body = (char *)body, .status = http_status};
    GHashTable *values;
    int result = -1;
    if (body == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Device response and output are required");
        return -1;
    }
    *out = (GhmDeviceCode){0};
    values = parse_form(&response, error);
    if (values == NULL) goto done;
    if (response.status != 200L || field(values, "error") != NULL) {
        oauth_error(values, error);
        goto done;
    }
    const char *device_code = field(values, "device_code");
    const char *user_code = field(values, "user_code");
    const char *verification_uri = field(values, "verification_uri");
    if (device_code == NULL || user_code == NULL || verification_uri == NULL ||
        strcmp(verification_uri, "https://github.com/login/device") != 0) {
        ghm_error_set(error, GHM_ERROR_AUTH, "Invalid GitHub device authorization response");
        goto done;
    }
    out->device_code = strdup(device_code);
    out->user_code = strdup(user_code);
    out->verification_uri = strdup(verification_uri);
    out->interval = positive_seconds(field(values, "interval"), 5);
    out->expires_in = positive_seconds(field(values, "expires_in"), 900);
    if (out->device_code == NULL || out->user_code == NULL || out->verification_uri == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        ghm_device_code_clear(out);
        goto done;
    }
    result = 0;
done:
    if (values != NULL) g_hash_table_unref(values);
    return result;
}

int ghm_github_auth_poll(const char *client_id, const GhmDeviceCode *device,
                         GhmAuthPollStatus *status, GhmOAuthToken *token,
                         unsigned *next_interval, GhmError *error)
{
    GhmHttpResponse response = {0};
    char *escaped_device = NULL;
    char *body = NULL;
    int result = -1;
    if (!valid_client_id(client_id) || device == NULL || device->device_code == NULL ||
        status == NULL || token == NULL || next_interval == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Invalid GitHub device authorization state");
        return -1;
    }
    *token = (GhmOAuthToken){0};
    *next_interval = device->interval;
    escaped_device = g_uri_escape_string(device->device_code, NULL, TRUE);
    body = g_strdup_printf("client_id=%s&device_code=%s&grant_type=urn%%3Aietf%%3Aparams%%3Aoauth%%3Agrant-type%%3Adevice_code",
                           client_id, escaped_device);
    if (ghm_http_request("https://github.com/login/oauth/access_token", body,
                         "application/x-www-form-urlencoded", NULL, &response, error) != 0) goto done;
    result = ghm_auth_parse_poll_response(response.body, response.status, device->interval,
                                           status, token, next_interval, error);
done:
    ghm_http_response_clear(&response);
    if (body != NULL) memset(body, 0, strlen(body));
    g_free(body);
    if (escaped_device != NULL) memset(escaped_device, 0, strlen(escaped_device));
    g_free(escaped_device);
    return result;
}

int ghm_auth_parse_poll_response(const char *body, long http_status, unsigned current_interval,
                                 GhmAuthPollStatus *status, GhmOAuthToken *token,
                                 unsigned *next_interval, GhmError *error)
{
    GhmHttpResponse response = {.body = (char *)body, .status = http_status};
    GHashTable *values;
    int result = -1;
    if (body == NULL || status == NULL || token == NULL || next_interval == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "OAuth response and outputs are required");
        return -1;
    }
    *token = (GhmOAuthToken){0};
    *next_interval = current_interval;
    values = parse_form(&response, error);
    if (values == NULL) goto done;
    const char *code = field(values, "error");
    if (code == NULL && response.status == 200L) {
        if (token_from_form(values, token, error) != 0) goto done;
        *status = GHM_AUTH_SUCCESS;
    } else if (code != NULL && strcmp(code, "authorization_pending") == 0) {
        *status = GHM_AUTH_PENDING;
    } else if (code != NULL && strcmp(code, "slow_down") == 0) {
        *status = GHM_AUTH_SLOW_DOWN;
        *next_interval = positive_seconds(field(values, "interval"),
                                          current_interval > UINT_MAX - 5 ? UINT_MAX : current_interval + 5);
    } else if (code != NULL && strcmp(code, "expired_token") == 0) {
        *status = GHM_AUTH_EXPIRED;
    } else if (code != NULL && strcmp(code, "access_denied") == 0) {
        *status = GHM_AUTH_DENIED;
    } else {
        oauth_error(values, error);
        goto done;
    }
    result = 0;
done:
    if (values != NULL) g_hash_table_unref(values);
    return result;
}

int ghm_github_auth_refresh(const char *client_id, const char *refresh_token,
                            GhmOAuthToken *out, GhmError *error)
{
    GhmHttpResponse response = {0};
    GHashTable *values = NULL;
    char *escaped_refresh = NULL;
    char *body = NULL;
    int result = -1;
    if (!valid_client_id(client_id) || refresh_token == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Client ID and refresh token are required");
        return -1;
    }
    *out = (GhmOAuthToken){0};
    escaped_refresh = g_uri_escape_string(refresh_token, NULL, TRUE);
    body = g_strdup_printf("client_id=%s&refresh_token=%s&grant_type=refresh_token", client_id, escaped_refresh);
    if (ghm_http_request("https://github.com/login/oauth/access_token", body,
                         "application/x-www-form-urlencoded", NULL, &response, error) != 0) goto done;
    values = parse_form(&response, error);
    if (values == NULL) goto done;
    if (response.status != 200L || field(values, "error") != NULL) {
        oauth_error(values, error);
        goto done;
    }
    result = token_from_form(values, out, error);
done:
    if (values != NULL) g_hash_table_unref(values);
    ghm_http_response_clear(&response);
    if (body != NULL) memset(body, 0, strlen(body));
    g_free(body);
    if (escaped_refresh != NULL) memset(escaped_refresh, 0, strlen(escaped_refresh));
    g_free(escaped_refresh);
    return result;
}
