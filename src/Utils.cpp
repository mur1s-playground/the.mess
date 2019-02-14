#include "Utils.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

char *util_issue_command(const char *cmd) {
    char *result;
    result = (char *) malloc(sizeof(char)*256);
    char buffer[256];
    FILE* pipe = popen(cmd, "r");
    if (!pipe) return NULL;
    while (!feof(pipe)) {
        if (fgets(buffer, 256, pipe) != NULL) {
                for (int i = 0; i < 256; i++) {
                        result[i] = buffer[i];
                        if (buffer[i] == '\0') break;
//                      printf("%c", buffer[i]);
                }
        }
    }
    pclose(pipe);
    return result;
}

int util_get_netiface_id() {
	char *net_id = util_issue_command("ip maddr show | grep eth0 | sed 's/:/\\n/g' | head -n1");
//	printf("%i\r\n", strlen(net_id));
	if (net_id == NULL || strlen(net_id) == 0) {
		if (net_id != NULL) {
			free(net_id);
		}
		net_id = util_issue_command("ip maddr show | grep wlan0 | sed 's/:/\\n/g' | head -n1");
//		printf("%i\r\n", strlen(net_id));
	}
	int id = 0;
	if (net_id == NULL) {

	} else if (strlen(net_id) == 2) {
		id = net_id[0] - '0';
	} else if (strlen(net_id) == 3) {
		id = (net_id[0] - '0')*10 + (net_id[1] - '0');
	}
	if (net_id != NULL) {
		free(net_id);
	}
	printf("net_id : %i\r\n", id);
	return id;
}
