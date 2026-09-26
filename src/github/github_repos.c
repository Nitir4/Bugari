#include "github/github_repos.h"
#include "github/github_api.h"
#include "core/git.h"

#include <json-glib/json-glib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ghm_github_repos_free(GhmGitHubRepoList *list)
{
    if (list == NULL) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].full_name);
        free(list->items[i].clone_url);
        free(list->items[i].default_branch);
        free(list->items[i].description);
        free(list->items[i].html_url);
    }
    free(list->items);
    *list = (GhmGitHubRepoList){0};
}

static const char *string_member(JsonObject *object, const char *key)
{
    JsonNode *node = json_object_get_member(object, key);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_STRING) return NULL;
    return json_node_get_string(node);
}

static int64_t nonnegative_count(JsonObject *object, const char *key)
{
    JsonNode *node = json_object_get_member(object, key);
    if (node == NULL || !JSON_NODE_HOLDS_VALUE(node) ||
        json_node_get_value_type(node) != G_TYPE_INT64) return 0;
    int64_t value = json_object_get_int_member(object, key);
    return value >= 0 ? value : 0;
}

static int append_repo(GhmGitHubRepoList *out, JsonObject *object, GhmError *error)
{
    /* Ownership of each successfully appended field belongs to out. */
    const char *full_name = string_member(object, "full_name");
    const char *clone_url = string_member(object, "clone_url");
    const char *default_branch = string_member(object, "default_branch");
    const char *description = string_member(object, "description");
    const char *html_url = string_member(object, "html_url");
    JsonNode *id_node = json_object_get_member(object, "id");
    JsonNode *private_node = json_object_get_member(object, "private");
    GhmGitHubRepo *grown;
    GhmGitHubRepo *repo;
    char *full_copy;
    char *url_copy;
    char *branch_copy;
    char *description_copy;
    char *web_copy;
    if (full_name == NULL || clone_url == NULL || id_node == NULL ||
        !JSON_NODE_HOLDS_VALUE(id_node) ||
        json_node_get_value_type(id_node) != G_TYPE_INT64 || private_node == NULL ||
        !JSON_NODE_HOLDS_VALUE(private_node) ||
        json_node_get_value_type(private_node) != G_TYPE_BOOLEAN) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned incomplete repository metadata");
        return -1;
    }
    int64_t github_id = json_object_get_int_member(object, "id");
    if (github_id <= 0) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned invalid repository metadata");
        return -1;
    }
    full_copy = strdup(full_name);
    if (full_copy == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); return -1; }
    url_copy = strdup(clone_url);
    if (url_copy == NULL) {
        free(full_copy);
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    branch_copy = strdup(default_branch != NULL ? default_branch : "");
    if (branch_copy == NULL) {
        free(url_copy);
        free(full_copy);
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    description_copy = strdup(description != NULL ? description : "");
    web_copy = strdup(html_url != NULL ? html_url : "");
    if (description_copy == NULL || web_copy == NULL) {
        free(web_copy);
        free(description_copy);
        free(branch_copy);
        free(url_copy);
        free(full_copy);
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    grown = realloc(out->items, (out->count + 1) * sizeof(*grown));
    if (grown == NULL) {
        free(web_copy);
        free(description_copy);
        free(branch_copy);
        free(url_copy);
        free(full_copy);
        ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
        return -1;
    }
    out->items = grown;
    repo = &out->items[out->count];
    *repo = (GhmGitHubRepo){0};
    repo->github_id = github_id;
    repo->full_name = full_copy;
    repo->clone_url = url_copy;
    repo->default_branch = branch_copy;
    repo->description = description_copy;
    repo->html_url = web_copy;
    repo->stargazers_count = nonnegative_count(object, "stargazers_count");
    repo->forks_count = nonnegative_count(object, "forks_count");
    repo->is_private = json_object_get_boolean_member(object, "private");
    repo->is_archived = json_object_has_member(object, "archived") &&
                        json_object_get_boolean_member(object, "archived");
    ++out->count;
    return 0;
}

int ghm_github_repos_parse_page(const char *json, GhmGitHubRepoList *out,
                                size_t *page_count, GhmError *error)
{
    JsonParser *parser = NULL;
    GError *parse_error = NULL;
    JsonNode *root;
    JsonArray *array;
    int result = -1;
    if (json == NULL || out == NULL || page_count == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository response and output are required");
        return -1;
    }
    *page_count = 0;
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, &parse_error)) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned invalid repository JSON");
        goto done;
    }
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned an invalid repository list");
        goto done;
    }
    array = json_node_get_array(root);
    size_t count = json_array_get_length(array);
    if (count > 100) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned too many repositories in one page");
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        JsonNode *node = json_array_get_element(array, (guint)i);
        if (!JSON_NODE_HOLDS_OBJECT(node) ||
            append_repo(out, json_node_get_object(node), error) != 0) {
            if (error != NULL && error->code == GHM_OK)
                ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned invalid repository metadata");
            goto done;
        }
    }
    *page_count = count;
    result = 0;
done:
    g_clear_error(&parse_error);
    g_clear_object(&parser);
    return result;
}

int ghm_github_repos_list(const char *client_id, GhmGitHubRepoList *out, GhmError *error)
{
    char *token = NULL;
    int result = -1;
    if (out == NULL) { ghm_error_set(error, GHM_ERROR_ARGUMENT, "Repository output is required"); return -1; }
    *out = (GhmGitHubRepoList){0};
    if (ghm_credentials_access_token(client_id, &token, error) != 0) return -1;
    for (unsigned page = 1; page <= 100; ++page) {
        char url[256];
        GhmHttpResponse response = {0};
        size_t page_count = 0;
        (void)snprintf(url, sizeof(url),
                       "https://api.github.com/user/repos?per_page=100&page=%u&sort=full_name", page);
        if (ghm_http_request(url, NULL, "application/vnd.github+json", token,
                             &response, error) != 0) goto done;
        if (response.status != 200L) {
            ghm_error_set(error, response.status == 401L ? GHM_ERROR_AUTH : GHM_ERROR_NETWORK,
                          response.status == 401L ? "GitHub login expired; sign in again" :
                          response.status == 403L || response.status == 429L ?
                          "GitHub rate limit or repository access denied" :
                          "GitHub could not list repositories");
            ghm_http_response_clear(&response);
            goto done;
        }
        int parsed = ghm_github_repos_parse_page(response.body, out, &page_count, error);
        ghm_http_response_clear(&response);
        if (parsed != 0) goto done;
        if (page_count < 100) { result = 0; goto done; }
    }
    ghm_error_set(error, GHM_ERROR_NETWORK, "Repository list exceeds the supported page limit");
done:
    ghm_access_token_free(token);
    if (result != 0) ghm_github_repos_free(out);
    return result;
}

void ghm_github_branches_free(GhmGitHubBranchList *list)
{
    if (list == NULL) return;
    for (size_t i = 0; i < list->count; ++i) {
        free(list->items[i].name);
        free(list->items[i].head_oid);
    }
    free(list->items);
    *list = (GhmGitHubBranchList){0};
}

int ghm_github_branches_parse_page(const char *json, GhmGitHubBranchList *out,
                                   size_t *page_count, GhmError *error)
{
    JsonParser *parser = NULL;
    GError *parse_error = NULL;
    JsonNode *root;
    JsonArray *array;
    int result = -1;
    if (json == NULL || out == NULL || page_count == NULL) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Branch response and output are required");
        return -1;
    }
    *page_count = 0;
    parser = json_parser_new();
    if (!json_parser_load_from_data(parser, json, -1, &parse_error)) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned invalid branch JSON");
        goto done;
    }
    root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_ARRAY(root)) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned an invalid branch list");
        goto done;
    }
    array = json_node_get_array(root);
    size_t count = json_array_get_length(array);
    if (count > 100) {
        ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned too many branches in one page");
        goto done;
    }
    for (size_t i = 0; i < count; ++i) {
        JsonNode *node = json_array_get_element(array, (guint)i);
        JsonObject *object, *commit;
        const char *name, *sha;
        GhmGitHubBranch *grown, *branch;
        if (!JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
        object = json_node_get_object(node);
        name = string_member(object, "name");
        if (name == NULL || name[0] == '\0' || !json_object_has_member(object, "commit") ||
            !JSON_NODE_HOLDS_OBJECT(json_object_get_member(object, "commit"))) goto invalid;
        commit = json_object_get_object_member(object, "commit");
        sha = string_member(commit, "sha");
        if (sha == NULL || strlen(sha) != 40) goto invalid;
        for (size_t digit = 0; digit < 40; ++digit)
            if (!isxdigit((unsigned char)sha[digit])) goto invalid;
        grown = realloc(out->items, (out->count + 1) * sizeof(*grown));
        if (grown == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
        out->items = grown;
        char *name_copy = strdup(name);
        if (name_copy == NULL) {
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            goto done;
        }
        char *sha_copy = strdup(sha);
        if (sha_copy == NULL) {
            free(name_copy);
            ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory");
            goto done;
        }
        branch = &out->items[out->count];
        *branch = (GhmGitHubBranch){0};
        branch->name = name_copy;
        branch->head_oid = sha_copy;
        branch->is_protected = json_object_has_member(object, "protected") &&
                               json_object_get_boolean_member(object, "protected");
        ++out->count;
    }
    *page_count = count;
    result = 0;
    goto done;
invalid:
    ghm_error_set(error, GHM_ERROR_NETWORK, "GitHub returned incomplete branch metadata");
done:
    g_clear_error(&parse_error);
    g_clear_object(&parser);
    return result;
}

static int valid_repo_name(const char *full_name)
{
    if (full_name == NULL) return 0;
    const char *slash = strchr(full_name, '/');
    if (slash == NULL || slash == full_name || slash[1] == '\0' ||
        strchr(slash + 1, '/') != NULL) return 0;
    if ((slash == full_name + 1 && full_name[0] == '.') ||
        (slash == full_name + 2 && full_name[0] == '.' && full_name[1] == '.') ||
        strcmp(slash + 1, ".") == 0 || strcmp(slash + 1, "..") == 0) return 0;
    for (const char *cursor = full_name; *cursor != '\0'; ++cursor) {
        char ch = *cursor;
        if (ch == '/') continue;
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.')) return 0;
    }
    return 1;
}

int ghm_github_branches_list(const char *client_id, const char *full_name,
                             GhmGitHubBranchList *out, GhmError *error)
{
    char *token = NULL;
    int result = -1;
    if (out == NULL || !valid_repo_name(full_name)) {
        ghm_error_set(error, GHM_ERROR_ARGUMENT, "Valid OWNER/REPOSITORY is required");
        return -1;
    }
    *out = (GhmGitHubBranchList){0};
    if (ghm_credentials_access_token(client_id, &token, error) != 0) return -1;
    for (unsigned page = 1; page <= 100; ++page) {
        GhmHttpResponse response = {0};
        size_t page_count = 0;
        size_t url_length = strlen(full_name) + 100U;
        char *url = malloc(url_length);
        if (url == NULL) { ghm_error_set(error, GHM_ERROR_MEMORY, "Out of memory"); goto done; }
        (void)snprintf(url, url_length,
                       "https://api.github.com/repos/%s/branches?per_page=100&page=%u", full_name, page);
        int fetched = ghm_http_request(url, NULL, "application/vnd.github+json", token,
                                       &response, error);
        free(url);
        if (fetched != 0) goto done;
        if (response.status != 200L) {
            ghm_error_set(error, response.status == 401L ? GHM_ERROR_AUTH : GHM_ERROR_NETWORK,
                          response.status == 401L ? "GitHub login expired; sign in again" :
                          response.status == 403L || response.status == 429L ?
                          "GitHub rate limit or branch access denied" :
                          response.status == 404L ? "GitHub repository was not found or access was denied" :
                          "GitHub could not list branches");
            ghm_http_response_clear(&response);
            goto done;
        }
        int parsed = ghm_github_branches_parse_page(response.body, out, &page_count, error);
        ghm_http_response_clear(&response);
        if (parsed != 0) goto done;
        if (page_count < 100) { result = 0; goto done; }
    }
    ghm_error_set(error, GHM_ERROR_NETWORK, "Branch list exceeds the supported page limit");
done:
    ghm_access_token_free(token);
    if (result != 0) ghm_github_branches_free(out);
    return result;
}
