#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "../qxpresult.h"

static void report_stage(const char *stage, unsigned leg, DWORD value)
{
    char message[192];

    _snprintf(message, sizeof(message),
              "QXP_D3DVS_SUCCESSION_STAGE stage=%s leg=%u value=%lu",
              stage, leg, (unsigned long)value);
    message[sizeof(message) - 1] = '\0';
    puts(message);
    /* Do not hold COM1 open while a child probe is running. Each child owns
     * the serial reporter for its lifetime; the wrapper reports only the
     * final aggregate result after both handles have closed. */
}

static void report_result(int rc, const char *stage, unsigned leg,
                          DWORD value)
{
    char message[224];

    _snprintf(message, sizeof(message),
              "QXP_D3DVS_SUCCESSION_RESULT rc=%d stage=%s leg=%u value=%lu%s",
              rc, stage, leg, (unsigned long)value,
              rc ? "" : " legs=2 lifetime=delete-recreate");
    message[sizeof(message) - 1] = '\0';
    puts(message);
    qxp_report_serial(message);
}

static int run_leg(const char *probe, const char *directory, unsigned leg)
{
    STARTUPINFOA startup;
    PROCESS_INFORMATION process;
    char command[MAX_PATH * 2];
    DWORD wait_result;
    DWORD exit_code = 0xffffffffUL;

    _snprintf(command, sizeof(command), "\"%s\" --succession-child-%u",
              probe, leg);
    command[sizeof(command) - 1] = '\0';
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    memset(&process, 0, sizeof(process));
    report_stage("launch", leg, 0);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL,
                        directory, &startup, &process)) {
        return (int)GetLastError();
    }
    CloseHandle(process.hThread);
    wait_result = WaitForSingleObject(process.hProcess, 120000);
    if (wait_result == WAIT_OBJECT_0) {
        if (!GetExitCodeProcess(process.hProcess, &exit_code)) {
            exit_code = GetLastError();
        }
    } else {
        exit_code = wait_result == WAIT_TIMEOUT ? WAIT_TIMEOUT : GetLastError();
    }
    CloseHandle(process.hProcess);
    report_stage("exit", leg, exit_code);
    return (int)exit_code;
}

int main(void)
{
    char executable[MAX_PATH];
    char directory[MAX_PATH];
    char probe[MAX_PATH];
    char *separator;
    int rc;

    if (!GetModuleFileNameA(NULL, executable, sizeof(executable))) {
        report_result(1, "module-path", 0, GetLastError());
        return 1;
    }
    executable[sizeof(executable) - 1] = '\0';
    separator = strrchr(executable, '\\');
    if (!separator) {
        report_result(2, "module-directory", 0, 0);
        return 2;
    }
    *separator = '\0';
    lstrcpynA(directory, executable, sizeof(directory));
    _snprintf(probe, sizeof(probe), "%s\\qxpd3dvsprobe.exe", directory);
    probe[sizeof(probe) - 1] = '\0';

    rc = run_leg(probe, directory, 1);
    if (rc) {
        report_result(3, "first-child", 1, (DWORD)rc);
        return 3;
    }
    Sleep(1000);
    rc = run_leg(probe, directory, 2);
    if (rc) {
        report_result(4, "second-child", 2, (DWORD)rc);
        return 4;
    }
    report_result(0, "complete", 2, 0);
    return 0;
}
