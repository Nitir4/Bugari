#include <ghm/github.h>
#include "core/git.h"

#include <json-glib/json-glib.h>
#ifdef _WIN32
#include <windows.h>
#include <wincred.h>
#else
#include <libsecret/secret.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
static WCHAR *credential_name(const char *id)
{
    char *target = g_strdup_printf("io.github.ghm.oauth:%s", id);
    WCHAR *name = (WCHAR *)g_utf8_to_utf16(target, -1, NULL, NULL, NULL);
    g_free(target); return name;
}
static gboolean credential_save(const char *id, const char *text, GError **error)
{
    WCHAR *name = credential_name(id);
    size_t size = strlen(text);
    gboolean ok = FALSE;
    if (name != NULL && size <= CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        CREDENTIALW credential = {.Type = CRED_TYPE_GENERIC, .TargetName = name,
            .CredentialBlobSize = (DWORD)size, .CredentialBlob = (LPBYTE)text,
            .Persist = CRED_PERSIST_LOCAL_MACHINE, .UserName = L"GitHub OAuth"};
        ok = CredWriteW(&credential, 0);
    }
    g_free(name);
    if (!ok) g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
        "Cannot save login in Windows Credential Manager");
    return ok;
}
static char *credential_get(const char *id, GError **error)
{
    WCHAR *name = credential_name(id);
    PCREDENTIALW credential = NULL;
    char *text = NULL;
    if (name != NULL && CredReadW(name, CRED_TYPE_GENERIC, 0, &credential)) {
        text = g_strndup((const char *)credential->CredentialBlob, credential->CredentialBlobSize);
        CredFree(credential);
    } else if (name == NULL || GetLastError() != ERROR_NOT_FOUND) {
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
            "Cannot read login from Windows Credential Manager");
    }
    g_free(name); return text;
}
static void credential_delete(const char *id, GError **error)
{
    WCHAR *name = credential_name(id);
    if (name == NULL || (!CredDeleteW(name, CRED_TYPE_GENERIC, 0) && GetLastError() != ERROR_NOT_FOUND))
        g_set_error_literal(error, G_FILE_ERROR, G_FILE_ERROR_FAILED,
            "Cannot remove login from Windows Credential Manager");
    g_free(name);
}
static void credential_free(char *text)
{
    if (text != NULL) { SecureZeroMemory(text, strlen(text)); g_free(text); }
}
#else
static const SecretSchema oauth_schema = {
    .name = "io.github.ghm.oauth",
    .flags = SECRET_SCHEMA_NONE,
    .attributes = {{"client-id", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL, 0}}
};

static gboolean credential_save(const char *id, const char *text, GError **error)
{
    return secret_password_store_sync(&oauth_schema, SECRET_COLLECTION_DEFAULT,
        "GitHub Commit Manager OAuth session", text, NULL, error, "client-id", id, NULL);
}
static char *credential_get(const char *id, GError **error)
{ return secret_password_lookup_sync(&oauth_schema, NULL, error, "client-id", id, NULL); }
static void credential_delete(const char *id, GError **error)
{ (void)secret_password_clear_sync(&oauth_schema, NULL, error, "client-id", id, NULL); }
static void credential_free(char *text) { secret_password_free(text); }
#endif

int ghm_credentials_store(const char *client_id, const GhmOAuthToken *token, GhmError *error)
{
    JsonObject *object;
    JsonNode *root;
    char *serialized;
    GError *secret_error = NULL;
    gboolean stored;
    if (client_id == NULL || client_id[0] == '\0' || token == NULL || token->access_token == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Client ID and access token are required");
        return -1;
    }
    object = json_object_new();
    json_object_set_string_member(object, "access_token", token->access_token);
    if (token->refresh_token != NULL)
        json_object_set_string_member(object, "refresh_token", token->refresh_token);
    json_object_set_int_member(object, "expires_at", token->expires_at);
    json_object_set_int_member(object, "refresh_expires_at", token->refresh_expires_at);
    root = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(root, object);
    serialized = json_to_string(root, FALSE);
    json_node_free(root);
    if (serialized == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); return -1; }
    stored = credential_save(client_id, serialized, &secret_error);
    volatile char *bytes = (volatile char *)serialized;
    for (size_t i = 0, length = strlen(serialized); i < length; ++i) bytes[i] = '\0';
    g_free(serialized);
    if (!stored) {
        ghm_error_set(error, GHM_ERROR_SECRET,
                      secret_error != NULL ? secret_error->message : "Cannot save GitHub login in desktop keyring");
        g_clear_error(&secret_error);
        return -1;
    }
    g_clear_error(&secret_error);
    return 0;
}

int ghm_credentials_load(const char *client_id, GhmOAuthToken *out, GhmError *error)
{
    gchar *serialized;
    GError *secret_error = NULL;
    GError *parse_error = NULL;
    JsonParser *parser = NULL;
    JsonNode *root;
    JsonObject *object;
    int result = -1;
    if (client_id == NULL || client_id[0] == '\0' || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Client ID and output token are required");
        return -1;
    }
    *out = (GhmOAuthToken){0};
    serialized = credential_get(client_id, &secret_error);
    if (secret_error != NULL) {
        ghm_error_set(error, GHM_ERROR_SECRET, secret_error->message);
        g_clear_error(&secret_error);
        return -1;
    }
    if (serialized == NULL) return 0; /* No saved login. */
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, serialized, -1, &parse_error)) {
        ghm_error_set(error, GHM_ERROR_SECRET, "Saved GitHub login is invalid");
        goto done;
    }
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        ghm_error_set(error, GHM_ERROR_SECRET, "Saved GitHub login is invalid");
        goto done;
    }
    object = json_node_get_object(root);
    if (!json_object_has_member(object, "access_token")) {
        ghm_error_set(error, GHM_ERROR_SECRET, "Saved GitHub login is invalid");
        goto done;
    }
    const char *access = json_object_get_string_member(object, "access_token");
    const char *refresh = json_object_has_member(object, "refresh_token") ?
                          json_object_get_string_member(object, "refresh_token") : NULL;
    if (access == NULL || access[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_SECRET, "Saved GitHub login is invalid");
        goto done;
    }
    out->access_token = strdup(access);
    out->refresh_token = refresh != NULL ? strdup(refresh) : NULL;
    if (out->access_token == NULL || (refresh != NULL && out->refresh_token == NULL)) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        ghm_oauth_token_clear(out);
        goto done;
    }
    out->expires_at = json_object_has_member(object, "expires_at") ?
                      json_object_get_int_member(object, "expires_at") : 0;
    out->refresh_expires_at = json_object_has_member(object, "refresh_expires_at") ?
                              json_object_get_int_member(object, "refresh_expires_at") : 0;
    result = 0;
done:
    g_clear_error(&parse_error);
    g_clear_object(&parser);
    credential_free(serialized);
    return result;
}

int ghm_credentials_clear(const char *client_id, GhmError *error)
{
    GError *secret_error = NULL;
    if (client_id == NULL || client_id[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Client ID is required");
        return -1;
    }
    credential_delete(client_id, &secret_error);
    if (secret_error != NULL) {
        ghm_error_set(error, GHM_ERROR_SECRET, secret_error->message);
        g_clear_error(&secret_error);
        return -1;
    }
    return 0;
}

void ghm_access_token_free(char *token)
{
    if (token == NULL) return;
    volatile char *bytes = (volatile char *)token;
    for (size_t i = 0, length = strlen(token); i < length; ++i) bytes[i] = '\0';
    free(token);
}

int ghm_credentials_access_token(const char *client_id, char **out_token, GhmError *error)
{
    GhmOAuthToken saved = {0};
    GhmOAuthToken renewed = {0};
    int result = -1;
    if (out_token == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Token output is required");
        return -1;
    }
    *out_token = NULL;
    if (ghm_credentials_load(client_id, &saved, error) != 0) goto done;
    if (saved.access_token == NULL) {
        ghm_error_set(error, GHM_ERROR_AUTH, "Sign in to GitHub before accessing repositories");
        goto done;
    }
    int64_t now = (int64_t)time(NULL);
    if (saved.expires_at > 0 && saved.expires_at <= now + 60) {
        if (saved.refresh_token == NULL ||
            (saved.refresh_expires_at > 0 && saved.refresh_expires_at <= now)) {
            ghm_error_set(error, GHM_ERROR_AUTH, "GitHub session expired; sign in again");
            goto done;
        }
        if (ghm_github_auth_refresh(client_id, saved.refresh_token, &renewed, error) != 0 ||
            ghm_credentials_store(client_id, &renewed, error) != 0) goto done;
        ghm_oauth_token_clear(&saved);
        saved = renewed;
        renewed = (GhmOAuthToken){0};
    }
    *out_token = saved.access_token;
    saved.access_token = NULL;
    result = 0;
done:
    ghm_oauth_token_clear(&renewed);
    ghm_oauth_token_clear(&saved);
    return result;
}
