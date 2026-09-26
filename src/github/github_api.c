#include "github/github_api.h"
#include "core/git.h"
#include "core/fault.h"
#include <ghm/github.h>

#include <curl/curl.h>
#include <json-glib/json-glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>

void ghm_http_read_header(GhmHttpResponse *response, const char *line, size_t length)
{
    if (length >= 5 && strncmp(line, "HTTP/", 5) == 0) {
        response->rate_remaining_zero = 0; response->retry_at = 0; response->rate_reset_at = 0; return;
    }
    const char *colon = memchr(line, ':', length);
    if (colon == NULL || (size_t)(colon - line) > 40) return;
    size_t name_length = (size_t)(colon - line);
    size_t value_length = length - name_length - 1;
    if (value_length >= 128) return;
    char value[128];
    memcpy(value, colon + 1, value_length); value[value_length] = '\0';
    char *end = NULL;
    errno = 0;
    unsigned long long number = strtoull(value, &end, 10);
    int digits = end != value;
    while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) ++end;
    int numeric = errno == 0 && digits && end != NULL && *end == '\0' && number <= INT64_MAX && strchr(value, '-') == NULL;
    if (name_length == 21 && strncasecmp(line, "x-ratelimit-remaining", name_length) == 0 && numeric)
        response->rate_remaining_zero = number == 0;
    if (name_length == 17 && strncasecmp(line, "x-ratelimit-reset", name_length) == 0 && numeric) {
        response->rate_reset_at = (int64_t)number;
    }
    if (name_length == 11 && strncasecmp(line, "retry-after", name_length) == 0) {
        int64_t now = (int64_t)time(NULL);
        int64_t retry_at = numeric && number <= (unsigned long long)(INT64_MAX - now) ? now + (int64_t)number :
                           (int64_t)curl_getdate(value, NULL);
        if (retry_at > response->retry_at) response->retry_at = retry_at;
    }
}

static size_t receive_header(char *line, size_t size, size_t count, void *payload)
{
    if (size != 0 && count > SIZE_MAX / size) return 0;
    size_t length = size * count;
    ghm_http_read_header(payload, line, length);
    return length;
}

typedef struct {
    char *data;
    size_t size;
    int exhausted;
} ResponseBuffer;

static int append_header(struct curl_slist **headers, const char *value, GhmError *error)
{
    struct curl_slist *updated = curl_slist_append(*headers, value);
    if (updated == NULL) {
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    *headers = updated;
    return 0;
}

static size_t receive_data(char *input, size_t size, size_t count, void *user_data)
{
    ResponseBuffer *buffer = user_data;
    size_t incoming;
    char *grown;
    if (size != 0 && count > SIZE_MAX / size) return 0;
    incoming = size * count;
    if (incoming > 1024U * 1024U - buffer->size) { buffer->exhausted = 1; return 0; }
    grown = realloc(buffer->data, buffer->size + incoming + 1);
    if (grown == NULL) { buffer->exhausted = 1; return 0; }
    buffer->data = grown;
    memcpy(buffer->data + buffer->size, input, incoming);
    buffer->size += incoming;
    buffer->data[buffer->size] = '\0';
    return incoming;
}

int ghm_http_request(const char *url, const char *post_body, const char *accept,
                     const char *bearer_token, GhmHttpResponse *out, GhmError *error)
{
    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    ResponseBuffer buffer = {0};
    char curl_error[CURL_ERROR_SIZE] = {0};
    char *authorization = NULL;
    CURLcode result;
    int success = -1;
    if (url == NULL || out == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "HTTP URL and output are required");
        return -1;
    }
    *out = (GhmHttpResponse){0};
    if (ghm_fault("http.request", error) != 0) return -1;
    curl = curl_easy_init();
    if (curl == NULL) { ghm_error_set(error, GHM_ERROR_NETWORK, "Cannot initialize network request"); return -1; }
    if (accept != NULL) {
        char *header = g_strdup_printf("Accept: %s", accept);
        int appended = append_header(&headers, header, error);
        g_free(header);
        if (appended != 0) goto done;
    }
    if (bearer_token != NULL) {
        authorization = g_strdup_printf("Authorization: Bearer %s", bearer_token);
        int appended = append_header(&headers, authorization, error);
        memset(authorization, 0, strlen(authorization));
        g_free(authorization);
        if (appended != 0) goto done;
    }
    if (post_body != NULL && append_header(&headers,
        "Content-Type: application/x-www-form-urlencoded", error) != 0) goto done;
    (void)curl_easy_setopt(curl, CURLOPT_URL, url);
    (void)curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "ghm/0.2");
    (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_data);
    (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    (void)curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, receive_header);
    (void)curl_easy_setopt(curl, CURLOPT_HEADERDATA, out);
    (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    (void)curl_easy_setopt(curl, CURLOPT_TIMEOUT, 25L);
    (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    /* Portable builds use the target desktop's current trust store. */
    const char *ca_bundle = getenv("GHM_CA_BUNDLE");
    if (ca_bundle != NULL && ca_bundle[0] != '\0')
        (void)curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
    if (post_body != NULL) {
        (void)curl_easy_setopt(curl, CURLOPT_POST, 1L);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_body);
        (void)curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(post_body));
    }
    result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        ghm_error_set(error, GHM_ERROR_NETWORK,
                      buffer.exhausted ? "GitHub response is too large or memory is exhausted" :
                      curl_error[0] != '\0' ? curl_error : curl_easy_strerror(result));
        goto done;
    }
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &out->status);
    out->body = buffer.data != NULL ? buffer.data : strdup("");
    buffer.data = NULL;
    if (out->body == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    if (out->status == 429 || (out->status == 403 && (out->rate_remaining_zero ||
        strstr(out->body, "rate limit") != NULL || strstr(out->body, "rate-limit") != NULL))) {
        int64_t retry_at = out->retry_at > (int64_t)time(NULL) ? out->retry_at : (int64_t)time(NULL) + 60;
        if (out->rate_remaining_zero && out->rate_reset_at > retry_at) retry_at = out->rate_reset_at;
        ghm_error_set(error, GHM_ERROR_RATE_LIMIT, "GitHub rate limit reached. Wait until the retry time before trying again");
        if (error != NULL) error->retry_at = retry_at;
        ghm_http_response_clear(out);
        goto done;
    }
    success = 0;
done:
    if (buffer.data != NULL) {
        volatile char *bytes = (volatile char *)buffer.data;
        for (size_t i = 0; i < buffer.size; ++i) bytes[i] = '\0';
    }
    free(buffer.data);
    for (struct curl_slist *item = headers; item != NULL; item = item->next) {
        if (item->data != NULL && strncmp(item->data, "Authorization:", 14) == 0) {
            volatile char *bytes = (volatile char *)item->data;
            size_t length = strlen(item->data);
            for (size_t i = 0; i < length; ++i) bytes[i] = '\0';
        }
    }
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return success;
}

void ghm_http_response_clear(GhmHttpResponse *response)
{
    if (response == NULL) return;
    if (response->body != NULL) {
        volatile char *bytes = (volatile char *)response->body;
        size_t length = strlen(response->body);
        for (size_t i = 0; i < length; ++i) bytes[i] = '\0';
    }
    free(response->body);
    *response = (GhmHttpResponse){0};
}

int ghm_github_current_user(const char *access_token, char **login, GhmError *error)
{
    GhmHttpResponse response = {0};
    JsonParser *parser = NULL;
    JsonNode *root;
    JsonObject *object;
    GError *parse_error = NULL;
    int result = -1;
    if (access_token == NULL || login == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Access token and output login are required");
        return -1;
    }
    *login = NULL;
    if (ghm_http_request("https://api.github.com/user", NULL,
                         "application/vnd.github+json", access_token, &response, error) != 0) return -1;
    if (response.status == 401L) {
        ghm_error_set(error, GHM_ERROR_AUTH, "GitHub rejected the stored login");
        goto done;
    }
    if (response.status == 403L) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub denied the profile request or hit an API rate limit");
        goto done;
    }
    if (response.status != 200L) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub could not provide the current user");
        goto done;
    }
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, response.body, -1, &parse_error)) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "Invalid GitHub user response");
        goto done;
    }
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "Invalid GitHub user response");
        goto done;
    }
    object = json_node_get_object(root);
    if (!json_object_has_member(object, "login")) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub response has no login");
        goto done;
    }
    const char *value = json_object_get_string_member(object, "login");
    if (value == NULL || value[0] == '\0') {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub response has no login");
        goto done;
    }
    *login = strdup(value);
    if (*login == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
    result = 0;
done:
    g_clear_error(&parse_error);
    g_clear_object(&parser);
    ghm_http_response_clear(&response);
    return result;
}
