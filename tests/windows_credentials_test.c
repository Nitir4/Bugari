#include <ghm/github.h>
#include <glib.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    /* A unique synthetic login exercises the backend without touching any
     * actual user's saved session. The target is cleared on every exit path. */
    char id[100];
    snprintf(id, sizeof(id), "GHMTest%lu%" G_GINT64_FORMAT,
        (unsigned long)GetCurrentProcessId(), g_get_real_time());
    GhmOAuthToken token = {.access_token = "synthetic-access", .refresh_token = "synthetic-refresh",
        .expires_at = 4102444800LL, .refresh_expires_at = 4102444900LL};
    GhmOAuthToken loaded = {0}; GhmError error = {0}; int result = 1;
    if (ghm_credentials_load(id, &loaded, &error) != 0 || loaded.access_token != NULL ||
        ghm_credentials_store(id, &token, &error) != 0 || ghm_credentials_load(id, &loaded, &error) != 0 ||
        loaded.access_token == NULL || loaded.refresh_token == NULL ||
        strcmp(loaded.access_token, token.access_token) != 0 || strcmp(loaded.refresh_token, token.refresh_token) != 0 ||
        loaded.expires_at != token.expires_at || loaded.refresh_expires_at != token.refresh_expires_at) goto done;
    ghm_oauth_token_clear(&loaded);
    if (ghm_credentials_clear(id, &error) != 0 || ghm_credentials_load(id, &loaded, &error) != 0 ||
        loaded.access_token != NULL || ghm_credentials_clear(id, &error) != 0) goto done;
    puts("PASS Windows Credential Manager synthetic login round trip and idempotent removal"); result = 0;
done:
    if (result) fprintf(stderr, "Credential backend test failed: %s\n", error.message);
    ghm_oauth_token_clear(&loaded); ghm_credentials_clear(id, &error); return result;
}
