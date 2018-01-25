#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "cutils/properties.h"

#define DEFAULT_ROTATE_SIZE_KB      (8192)      // 8MB
#define DEFAULT_MAX_FILE_NR         (4)
#define DEFAULT_PERIOD_US           (1000000)    // 1000ms

#define DEFAULT_FILE_NAME           "kernel.log"

#define FAILURE_RETRY(exp) ({               \
    typeof (exp) _rc;                       \
    do {                                    \
        _rc = (exp);                        \
    } while (_rc == -1 && errno == EINTR);  \
    _rc; })

static char g_outputFileName[128];
static int g_logRotateSizeKBytes = DEFAULT_ROTATE_SIZE_KB;
static int g_maxRotatedLogs = DEFAULT_MAX_FILE_NR;
static int g_outFD = -1;
static off_t g_outByteCount = 0;
static void (*logger_fn)(); // do_klogging_ksmg or do_klogging_dmesg
static bool g_printed = false;

static int openLogFile (const char *pathname)
{
    return open(pathname, O_WRONLY | O_APPEND | O_CREAT, S_IRUSR | S_IWUSR);
}

static void setupOutput()
{
    if (!strlen(g_outputFileName)) {
        g_outFD = STDOUT_FILENO;
    } else {
        struct stat statbuf;

        g_outFD = openLogFile (g_outputFileName);

        if (g_outFD < 0) {
            perror ("couldn't open output file");
            exit(-1);
        }

        fstat(g_outFD, &statbuf);

        g_outByteCount = statbuf.st_size;
    }
}

static void show_help(const char* cmd)
{
    fprintf(stderr, "Usage: %s <[options]>\n", cmd);
    fprintf(stderr, "options include:\n"
                    "  -f <filename>   Log to file\n"
                    "  -r [<kbytes>]   Rotate log every kbytes. (16 if unspecified). Requires -f\n"
                    "  -n <count>      Sets max number of rotated logs to <count>, default 4\n"
    );
    fprintf(stderr, "\n");
    fprintf(stderr, "example: 8kb, max 4 files, to /data/lckt/logging\n");
    fprintf(stderr, "$ klogcat -r 8192 -n 4 -f /data/lckt/logging\n\n");
}

void parse_args(int argc, char* argv[])
{
    if (argc == 2 && !strcmp(argv[1], "--help")) {
        show_help(argv[0]);
        exit(0);
    }

    char logging_dest[PROPERTY_VALUE_MAX];
    char file_name[PROPERTY_VALUE_MAX + sizeof(DEFAULT_FILE_NAME)];
    property_get("sys.lckt.logging.dest", logging_dest, "/data/lckt/logging");
    snprintf(file_name, sizeof(file_name), "%s/%s", logging_dest, DEFAULT_FILE_NAME);

    for (;;) {
        int ret;
        
        ret = getopt(argc, argv, "f:n:r:");

        if (ret < 0)
            break;

        switch(ret) {
            case 'f':
                // redirect ouput to a file
                strcpy(g_outputFileName, optarg);
                break;
            case 'n':
                if (!isdigit(optarg[0])) {
                    fprintf(stderr, "Invalid parameter to -n\n");
                    show_help(argv[0]);
                    exit(-1);
                }

                g_maxRotatedLogs = atoi(optarg);
                break;
            case 'r':
                if (optarg == NULL) {
                    g_logRotateSizeKBytes = DEFAULT_ROTATE_SIZE_KB;
                } else {
                    if (!isdigit(optarg[0])) {
                        fprintf(stderr, "Invalid parameter to -r\n");
                        show_help(argv[0]);
                        exit(-1);
                    }

                    g_logRotateSizeKBytes = atoi(optarg);
                }
                break;
        }
    }

    // check output file, then set g_outputFileName
    if (!strlen(g_outputFileName)) {
        fprintf(stderr, "Destination file name set to default! %s\n\n", file_name);
        strcpy(g_outputFileName, file_name);
    }

    return;
}

static void print_args()
{
    fprintf(stderr, "g_logRotateSizeKBytes=%d\n", g_logRotateSizeKBytes);
    fprintf(stderr, "g_maxRotatedLogs=%d\n", g_maxRotatedLogs);
    fprintf(stderr, "g_outputFileName=%s\n", g_outputFileName);

    return;
}

static int isFileExist(const char* fileName)
{
    struct stat info;
    return !stat(fileName, &info);
}

static void rotateLogs()
{
    int err;

    if (!strlen(g_outputFileName))
        return;

    close(g_outFD);

    for (int i = g_maxRotatedLogs; i > 0 ; i--) {
        char *file0, *file1;

        asprintf(&file1, "%s.%d", g_outputFileName, i);

        if (i - 1 == 0)
            asprintf(&file0, "%s", g_outputFileName);
        else
            asprintf(&file0, "%s.%d", g_outputFileName, i-1);
        
        err = rename(file0, file1);

        if (err < 0 && errno != ENOENT)
            perror("while rotating log files");
        
        free(file1);
        free(file0);
    }

    g_outFD = openLogFile(g_outputFileName);

    if (g_outFD < 0) {
        perror("could not open output file");
        exit(-1);
    }

    g_outByteCount = 0;

    return;
}

static void maybe_print_start()
{
    if (!g_printed) {
        g_printed = true;

        char buf[1024];
        snprintf(buf, sizeof(buf), "\n--------- beginning of %s\n",
                    g_outputFileName);
        if (write(g_outFD, buf, strlen(buf)) < 0) {
            perror("output error");
            exit(-1);
        }
    }
}

void do_klogging_dmesg()
{
    char cmd[128] = {0, };
    static int err_nr = 0;

    while (1) {
        struct stat log;

        fprintf(stderr, "do_klogging_dmesg()\n");

        // do logging
        sprintf(cmd, "dmesg -c >> %s", g_outputFileName);
        system(cmd);

        if (chmod(g_outputFileName, 0666) < 0) {
            perror("Error! chmod, May be file system is corrupted");

            if (err_nr++ > 10) {
                perror("Critical! Force stop logging");
                exit(-1);
            }
        }

        // check file size, then rorate
        if (!stat(g_outputFileName, &log)) {
            if (g_logRotateSizeKBytes > 0
                && (log.st_size / 1024) >= g_logRotateSizeKBytes)
                rotateLogs();
        }

        // go to sleep
        usleep(DEFAULT_PERIOD_US);
    }

    return;
}

static void do_klogging_ksmg()
{
    static int err_nr;
    int kmsg_fd;
    char buff[1024] = {0, };

retry:
    kmsg_fd = open("/proc/kmsg", O_RDONLY);
    if (kmsg_fd < 0) {
        perror("Error! Open failed. /proc/kmsg");
        exit(-1);
    }

    while (1) {
        int byteWritten;
        int count = FAILURE_RETRY(read(kmsg_fd, buff, sizeof(buff)));

        if (count < 0) {
            perror("Error! Read failed. /proc/kmsg");
            goto exit;
        } else if (count == 0) { /* EOF */
            perror("Warn! Go to retry /proc/kmsg");
            close(kmsg_fd);
            goto retry;
        }

        maybe_print_start();

        byteWritten = FAILURE_RETRY(write(g_outFD, buff, count));

        if (byteWritten < 0) {
            perror("output error");
            close(g_outFD);
            exit(-1);
        }

        g_outByteCount += byteWritten;

        if (chmod(g_outputFileName, 0666) < 0) {
            perror("Error! chmod, May be file system is corrupted");

            if (err_nr++ > 10) {
                perror("Critical! Force stop logging");
                exit(-1);
            }
        }

        if (g_logRotateSizeKBytes > 0
            && (g_outByteCount / 1024) >= g_logRotateSizeKBytes)
            rotateLogs();

        memset(buff, 0x00, count);
    }

exit:
    close(kmsg_fd);
    exit(-1);
}

static void setLogger() {
    logger_fn = do_klogging_ksmg;
}

int main(int argc, char *argv[])
{
    parse_args(argc, argv); 
    print_args();

    setupOutput();
    setLogger();
    logger_fn();

    return 0;
}