#include "bridge.hpp"
#include <stdio.h>
#include <vector>

int main (int argc, char *argv[]) {
    if (argc != 3) {
        fprintf(stderr, "Error: too few arguments given\n");
        fprintf(stderr, "Usage: device_locator [user] [pass]\n");
        return 1;
    }

    char* username = argv[1];
    char* password = argv[2];
    std::string authorization_header;
    std::string access_token;
    
    printf("Enumerating devices... please wait!\n");

    // Login with given credentials
    if (!login(username, password, authorization_header, &access_token)) {
        fprintf(stderr, "Login failed... shutting down\n");
        return 1;
    }

    // Get auth0 user id
    std::string auth0_userid;
    bool userid_result = auth0_get_userid(access_token, auth0_userid);
    if (!userid_result) {
        fprintf(stderr, "User ID lookup failed\n");
        return 1;
    }

    // Get list of devices
    std::vector<std::pair<std::string, std::string>> devices;
    bool device_enum_result = get_user_devices(authorization_header, auth0_userid, devices);

    if (!device_enum_result) {
        fprintf(stderr, "Device enumeration failed\n");
        return 1;
    }

    // Print device list
    printf("Listing devices for user:\n");
    for (int i = 0; i < devices.size(); i++) {
        printf("[%d] %s (%s)\n", i, devices[i].second.c_str(), devices[i].first.c_str());
    }

    return 0;
}
