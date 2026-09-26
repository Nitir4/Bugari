#include <ghm/github.h>
#include "github/github_auth.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static int check(int condition, const char *description)
{
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", description);
    return -1;
}

int main(void)
{
    GhmDeviceCode device = {0};
    GhmOAuthToken token = {0};
    GhmAuthPollStatus status = GHM_AUTH_PENDING;
    GhmError error = {0};
    unsigned interval = 0;
    time_t before = time(NULL);
    const char *device_response =
        "device_code=abcd1234&user_code=ABCD-EFGH&verification_uri=https%3A%2F%2Fgithub.com%2Flogin%2Fdevice"
        "&interval=5&expires_in=900";
    if (ghm_auth_parse_device_response(device_response, 200, &device, &error) != 0) goto failed;
    if (check(strcmp(device.user_code, "ABCD-EFGH") == 0 && device.interval == 5 &&
              device.expires_in == 900, "device code response") != 0) goto failed;
    if (ghm_auth_parse_poll_response("error=authorization_pending", 200, 5,
                                     &status, &token, &interval, &error) != 0) goto failed;
    if (check(status == GHM_AUTH_PENDING && interval == 5, "pending response") != 0) goto failed;
    if (ghm_auth_parse_poll_response("error=slow_down&interval=10", 200, 5,
                                     &status, &token, &interval, &error) != 0) goto failed;
    if (check(status == GHM_AUTH_SLOW_DOWN && interval == 10, "slow-down response") != 0) goto failed;
    if (ghm_auth_parse_poll_response("error=slow_down", 200, 10,
                                     &status, &token, &interval, &error) != 0) goto failed;
    if (check(status == GHM_AUTH_SLOW_DOWN && interval == 15, "slow-down fallback") != 0) goto failed;
    if (ghm_auth_parse_poll_response("access_token=test_access&refresh_token=test_refresh"
                                     "&expires_in=28800&refresh_token_expires_in=15897600",
                                     200, 5, &status, &token, &interval, &error) != 0) goto failed;
    if (check(status == GHM_AUTH_SUCCESS && strcmp(token.access_token, "test_access") == 0 &&
              strcmp(token.refresh_token, "test_refresh") == 0 &&
              token.expires_at >= (int64_t)before + 28800, "token response") != 0) goto failed;
    ghm_oauth_token_clear(&token);
    ghm_device_code_clear(&device);
    if (ghm_auth_parse_device_response("device_code=abc&user_code=DEF&verification_uri=https%3A%2F%2Fevil.example",
                                       200, &device, &error) == 0) {
        fprintf(stderr, "FAIL: invalid verification URI accepted\n");
        goto failed;
    }
    error = (GhmError){0};
    if (ghm_auth_parse_device_response("error=device_flow_disabled", 400, &device, &error) == 0 ||
        error.code != GHM_ERROR_AUTH) {
        fprintf(stderr, "FAIL: disabled device flow was not reported\n");
        goto failed;
    }
    ghm_device_code_clear(&device);
    return 0;
failed:
    if (error.message[0] != '\0') fprintf(stderr, "%s\n", error.message);
    ghm_device_code_clear(&device);
    ghm_oauth_token_clear(&token);
    return 1;
}
