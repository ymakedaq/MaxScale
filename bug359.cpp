#include <my_config.h>
#include <iostream>
#include <unistd.h>
#include "testconnections.h"

using namespace std;

int main()
{

    TestConnections * Test = new TestConnections();
    int global_result = 0;
    char * err_log_content;

    Test->ReadEnv();
    Test->PrintIP();

    printf("Trying to connect to MaxScale\n");
    global_result = Test->ConnectMaxscale();
    if (global_result != 0) {
        printf("Error opening connections to MaxScale\n");
    }

    printf("Getting logs\n");
    char sys1[4096];
    sprintf(&sys1[0], "%s %s", Test->GetLogsCommand, Test->Maxscale_IP);
    printf("Executing: %s\n", sys1);
    fflush(stdout);
    system(sys1);

    printf("Reading err_log\n");
    global_result += ReadLog((char *) "skygw_err1.log", &err_log_content);

    if (strstr(err_log_content, "Warning : Unsupported router option \"slave\"") == NULL) {
        global_result++;
        printf("There is NO \"Warning : Unsupported router option \"slave\" \" warning in the log\n");
    } else {
        printf("There is proper \"Warning : Unsupported router option \"slave\" \" warning in the log\n");
    }
    if (strstr(err_log_content, "Error : Couldn't find suitable Master") != NULL) {
        global_result++;
        printf("\"Error : Couldn't find suitable Master\" error is present in the log\n");
    }

    Test->CloseMaxscaleConn();

    return(global_result);
}
