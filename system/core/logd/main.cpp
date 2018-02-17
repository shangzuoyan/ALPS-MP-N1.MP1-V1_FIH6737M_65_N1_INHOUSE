/*
 * Copyright (C) 2012-2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/klog.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <cstdbool>
#include <memory>

#include <cutils/properties.h>
#include <cutils/sched_policy.h>
#include <cutils/sockets.h>
#include <log/event_tag_map.h>
#include <packagelistparser/packagelistparser.h>
#include <private/android_filesystem_config.h>
#include <utils/threads.h>

#include "CommandListener.h"
#include "LogBuffer.h"
#include "LogListener.h"
#include "LogAudit.h"
#include "LogKlog.h"
#include "LogUtils.h"

#define KMSG_PRIORITY(PRI)                            \
    '<',                                              \
    '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) / 10, \
    '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) % 10, \
    '>'

//
//  The service is designed to be run by init, it does not respond well
// to starting up manually. When starting up manually the sockets will
// fail to open typically for one of the following reasons:
//     EADDRINUSE if logger is running.
//     EACCESS if started without precautions (below)
//
// Here is a cookbook procedure for starting up logd manually assuming
// init is out of the way, pedantically all permissions and selinux
// security is put back in place:
//
//    setenforce 0
//    rm /dev/socket/logd*
//    chmod 777 /dev/socket
//        # here is where you would attach the debugger or valgrind for example
//    runcon u:r:logd:s0 /system/bin/logd </dev/null >/dev/null 2>&1 &
//    sleep 1
//    chmod 755 /dev/socket
//    chown logd.logd /dev/socket/logd*
//    restorecon /dev/socket/logd*
//    setenforce 1
//
// If minimalism prevails, typical for debugging and security is not a concern:
//
//    setenforce 0
//    chmod 777 /dev/socket
//    logd
//

int kernelLogFd = -1;

static int drop_privs() {
    struct sched_param param;
    memset(&param, 0, sizeof(param));

    if (set_sched_policy(0, SP_FOREGROUND) < 0) {
        return -1;
    }

    if (sched_setscheduler((pid_t) 0, SCHED_BATCH, &param) < 0) {
        return -1;
    }

    if (setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_FOREGROUND) < 0) {
        return -1;
    }

    if (prctl(PR_SET_KEEPCAPS, 1) < 0) {
        return -1;
    }

    gid_t groups[] = { AID_READPROC };

    if (setgroups(sizeof(groups) / sizeof(groups[0]), groups) == -1) {
        return -1;
    }

    if (setgid(AID_LOGD) != 0) {
        return -1;
    }

    if (setuid(AID_LOGD) != 0) {
        return -1;
    }

    struct __user_cap_header_struct capheader;
    struct __user_cap_data_struct capdata[2];
    memset(&capheader, 0, sizeof(capheader));
    memset(&capdata, 0, sizeof(capdata));
    capheader.version = _LINUX_CAPABILITY_VERSION_3;
    capheader.pid = 0;

    capdata[CAP_TO_INDEX(CAP_SYSLOG)].permitted = CAP_TO_MASK(CAP_SYSLOG);
    capdata[CAP_TO_INDEX(CAP_AUDIT_CONTROL)].permitted |= CAP_TO_MASK(CAP_AUDIT_CONTROL);

    capdata[0].effective = capdata[0].permitted;
    capdata[1].effective = capdata[1].permitted;
    capdata[0].inheritable = 0;
    capdata[1].inheritable = 0;

    if (capset(&capheader, &capdata[0]) < 0) {
        return -1;
    }

    return 0;
}

// Property helper
static bool check_flag(const char *prop, const char *flag) {
    const char *cp = strcasestr(prop, flag);
    if (!cp) {
        return false;
    }
    // We only will document comma (,)
    static const char sep[] = ",:;|+ \t\f";
    if ((cp != prop) && !strchr(sep, cp[-1])) {
        return false;
    }
    cp += strlen(flag);
    return !*cp || !!strchr(sep, *cp);
}

bool property_get_bool(const char *key, int flag) {
    char def[PROPERTY_VALUE_MAX];
    char property[PROPERTY_VALUE_MAX];
    def[0] = '\0';
    if (flag & BOOL_DEFAULT_FLAG_PERSIST) {
        char newkey[PROPERTY_KEY_MAX];
        snprintf(newkey, sizeof(newkey), "ro.%s", key);
        property_get(newkey, property, "");
        // persist properties set by /data require inoculation with
        // logd-reinit. They may be set in init.rc early and function, but
        // otherwise are defunct unless reset. Do not rely on persist
        // properties for startup-only keys unless you are willing to restart
        // logd daemon (not advised).
        snprintf(newkey, sizeof(newkey), "persist.%s", key);
        property_get(newkey, def, property);
    }

    property_get(key, property, def);

    if (check_flag(property, "true")) {
        return true;
    }
    if (check_flag(property, "false")) {
        return false;
    }
    if (check_flag(property, "eng")) {
       flag |= BOOL_DEFAULT_FLAG_ENG;
    }
    // this is really a "not" flag
    if (check_flag(property, "svelte")) {
       flag |= BOOL_DEFAULT_FLAG_SVELTE;
    }

    // Sanity Check
    if (flag & (BOOL_DEFAULT_FLAG_SVELTE | BOOL_DEFAULT_FLAG_ENG)) {
        flag &= ~BOOL_DEFAULT_FLAG_TRUE_FALSE;
        flag |= BOOL_DEFAULT_TRUE;
    }

    if ((flag & BOOL_DEFAULT_FLAG_SVELTE)
            && property_get_bool("ro.config.low_ram",
                                 BOOL_DEFAULT_FALSE)) {
        return false;
    }
    if (flag & BOOL_DEFAULT_FLAG_ENG) {
        property_get("ro.debuggable", property, "");
        if (strcmp(property, "1")) {
            return false;
        }
    }

    return (flag & BOOL_DEFAULT_FLAG_TRUE_FALSE) != BOOL_DEFAULT_FALSE;
}

// Remove the static, and use this variable
// globally for debugging if necessary. eg:
//   write(fdDmesg, "I am here\n", 10);
static int fdDmesg = -1;

static sem_t uidName;
static uid_t uid;
static char *name;

static sem_t reinit;
static bool reinit_running = false;
static LogBuffer *logBuf = NULL;

static bool package_list_parser_cb(pkg_info *info, void * /* userdata */) {

    bool rc = true;
    if (info->uid == uid) {
        name = strdup(info->name);
        // false to stop processing
        rc = false;
    }

    packagelist_free(info);
    return rc;
}

void kernel_log_print(const char *fmt, ...) {
    if (kernelLogFd < 0)
        return;
    if (fmt == NULL) {
        return;
    }

    va_list args;

    char *str = NULL;
    va_start(args, fmt);
    int rc = vasprintf(&str, fmt, args);
    va_end(args);

    if (rc < 0) {
        free(str);
        return;
    }

    if (kernelLogFd >= 0) {
     write(kernelLogFd, str, strlen(str)+1);
    }
    free(str);
}

#if defined(MTK_LOGD_FILTER)
static int log_reader_count = 0;
void logd_reader_del(void) {
    char property[PROPERTY_VALUE_MAX];

    if (log_reader_count == 1) {
        property_get("persist.log.tag", property, "I");
        property_set("log.tag", property);
        kernel_log_print("logd no log reader, set log level to %s!\n", property);
    }
    log_reader_count--;
}

void logd_reader_add(void) {
    char property[PROPERTY_VALUE_MAX];

    if (log_reader_count == 0) {
        property_get("persist.log.tag", property, "M");
        property_set("log.tag", property);
        kernel_log_print("logd first log reader, set log level to %s!\n", property);
    }
    log_reader_count++;
}

#endif

#if defined(HAVE_AEE_FEATURE) && defined(ANDROID_LOG_MUCH_COUNT)
int log_detect_value;
int log_much_delay_detect = 0;      // log much detect pause, may use double detect value
int build_type;     // eng:0, userdebug:1 user:2
int detect_time = 1;
#endif


static void *reinit_thread_start(void * /*obj*/) {
    prctl(PR_SET_NAME, "logd.daemon");
    set_sched_policy(0, SP_FOREGROUND);
    setpriority(PRIO_PROCESS, 0, ANDROID_PRIORITY_FOREGROUND);

    // If we are AID_ROOT, we should drop to AID_SYSTEM, if we are anything
    // else, we have even lesser privileges and accept our fate. Not worth
    // checking for error returns setting this thread's privileges.
    (void)setgid(AID_SYSTEM);
    (void)setuid(AID_SYSTEM);
    char property[PROPERTY_VALUE_MAX];
#if defined(HAVE_AEE_FEATURE) && defined(ANDROID_LOG_MUCH_COUNT)
    bool value;
    int count;
    int delay;
#endif

    while (reinit_running && !sem_wait(&reinit) && reinit_running) {

        // uidToName Privileged Worker
        if (uid) {
            name = NULL;

            packagelist_parse(package_list_parser_cb, NULL);

            uid = 0;
            sem_post(&uidName);
            continue;
        }

        if (fdDmesg >= 0) {
            static const char reinit_message[] = { KMSG_PRIORITY(LOG_INFO),
                'l', 'o', 'g', 'd', '.', 'd', 'a', 'e', 'm', 'o', 'n', ':',
                ' ', 'r', 'e', 'i', 'n', 'i', 't', '\n' };
            write(fdDmesg, reinit_message, sizeof(reinit_message));
        }

        // Anything that reads persist.<property>
        if (logBuf) {
            logBuf->init();
            logBuf->initPrune(NULL);
        }

#if defined(HAVE_AEE_FEATURE) && defined(ANDROID_LOG_MUCH_COUNT)
        property_get("ro.aee.build.info", property, "");
        value = !strcmp(property, "mtk");
        if (value != true) {
            log_detect_value = 0;
            continue;
        }

        value = property_get_bool("persist.logmuch.detect", true);
        if (value == true) {
            property_get("ro.build.type", property, "");
            if (!strcmp(property, "eng")) {
                build_type = 0;
            } else if (!strcmp(property, "userdebug")) {
                build_type = 1;
            } else {
                build_type = 2;
            }

            if (log_detect_value == 0) {
                log_detect_value = ANDROID_LOG_MUCH_COUNT;
            }

            property_get("logmuch.detect.value", property, "-1");
            count = atoi(property);
            if (count == 0) {
                count = ANDROID_LOG_MUCH_COUNT;
            }
            kernel_log_print("logd: logmuch detect, build type %d, detect value %d:%d.\n",
                build_type, count, log_detect_value);

            if (count > 0 && count != log_detect_value) {  // set new log level
                log_detect_value = count;
                log_much_delay_detect = 1;
            }
            if (log_detect_value > 1000) {
                detect_time = 1;
            } else {
                detect_time = 6;
            }
            property_get("logmuch.detect.delay", property, "");
            delay = atoi(property);

            if (delay > 0) {
                log_much_delay_detect = 3*60;
                property_set("logmuch.detect.delay", "0");
            }
        } else {
            log_detect_value = 0;
            kernel_log_print("logd: logmuch detect disable.");
        }
#endif
#if defined(MTK_LOGD_FILTER)    /*for default status */
        if (log_reader_count == 0) {
            property_set("log.tag", "I");
            kernel_log_print("logd no log reader, set log level to INFO!\n");
        }
#endif
    }

    return NULL;
}

static sem_t sem_name;

char *android::uidToName(uid_t u) {
    if (!u || !reinit_running) {
        return NULL;
    }

    sem_wait(&sem_name);

    // Not multi-thread safe, we use sem_name to protect
    uid = u;

    name = NULL;
    sem_post(&reinit);
    sem_wait(&uidName);
    char *ret = name;

    sem_post(&sem_name);

    return ret;
}

// Serves as a global method to trigger reinitialization
// and as a function that can be provided to signal().
void reinit_signal_handler(int /*signal*/) {
    sem_post(&reinit);
}

// tagToName converts an events tag into a name
const char *android::tagToName(uint32_t tag) {
    static const EventTagMap *map;

    if (!map) {
        sem_wait(&sem_name);
        if (!map) {
            map = android_openEventTagMap(EVENT_TAG_MAP_FILE);
        }
        sem_post(&sem_name);
        if (!map) {
            return NULL;
        }
    }
    return android_lookupEventTag(map, tag);
}

static void readDmesg(LogAudit *al, LogKlog *kl) {
    if (!al && !kl) {
        return;
    }

    int rc = klogctl(KLOG_SIZE_BUFFER, NULL, 0);
    if (rc <= 0) {
        return;
    }

    size_t len = rc + 1024; // Margin for additional input race or trailing nul
    std::unique_ptr<char []> buf(new char[len]);

    rc = klogctl(KLOG_READ_ALL, buf.get(), len);
    if (rc <= 0) {
        return;
    }

    if ((size_t)rc < len) {
        len = rc + 1;
    }
    buf[--len] = '\0';

    if (kl && kl->isMonotonic()) {
        kl->synchronize(buf.get(), len);
    }

    size_t sublen;
    for (char *ptr = NULL, *tok = buf.get();
         (rc >= 0) && ((tok = log_strntok_r(tok, &len, &ptr, &sublen)));
         tok = NULL) {
        if (al) {
            rc = al->log(tok, sublen);
        }
        if (kl) {
            rc = kl->log(tok, sublen);
        }
    }
}

// Foreground waits for exit of the main persistent threads
// that are started here. The threads are created to manage
// UNIX domain client sockets for writing, reading and
// controlling the user space logger, and for any additional
// logging plugins like auditd and restart control. Additional
// transitory per-client threads are created for each reader.
int main(int argc, char *argv[]) {
    int fdPmesg = -1;
    bool klogd = property_get_bool("logd.kernel",
                                   BOOL_DEFAULT_TRUE |
                                   BOOL_DEFAULT_FLAG_PERSIST |
                                   BOOL_DEFAULT_FLAG_ENG |
                                   BOOL_DEFAULT_FLAG_SVELTE);
    if (klogd) {
        fdPmesg = open("/proc/kmsg", O_RDONLY | O_NDELAY);
    }
    fdDmesg = open("/dev/kmsg", O_WRONLY);
    kernelLogFd = fdDmesg;

    // issue reinit command. KISS argument parsing.
    if ((argc > 1) && argv[1] && !strcmp(argv[1], "--reinit")) {
        int sock = TEMP_FAILURE_RETRY(
            socket_local_client("logd",
                                ANDROID_SOCKET_NAMESPACE_RESERVED,
                                SOCK_STREAM));
        if (sock < 0) {
            return -errno;
        }
        static const char reinit[] = "reinit";
        ssize_t ret = TEMP_FAILURE_RETRY(write(sock, reinit, sizeof(reinit)));
        if (ret < 0) {
            return -errno;
        }
        struct pollfd p;
        memset(&p, 0, sizeof(p));
        p.fd = sock;
        p.events = POLLIN;
        ret = TEMP_FAILURE_RETRY(poll(&p, 1, 1000));
        if (ret < 0) {
            return -errno;
        }
        if ((ret == 0) || !(p.revents & POLLIN)) {
            return -ETIME;
        }
        static const char success[] = "success";
        char buffer[sizeof(success) - 1];
        memset(buffer, 0, sizeof(buffer));
        ret = TEMP_FAILURE_RETRY(read(sock, buffer, sizeof(buffer)));
        if (ret < 0) {
            return -errno;
        }
        return strncmp(buffer, success, sizeof(success) - 1) != 0;
    }

    // Reinit Thread
    sem_init(&reinit, 0, 0);
    sem_init(&uidName, 0, 0);
    sem_init(&sem_name, 0, 1);
    pthread_attr_t attr;
    if (!pthread_attr_init(&attr)) {
        struct sched_param param;

        memset(&param, 0, sizeof(param));
        pthread_attr_setschedparam(&attr, &param);
        pthread_attr_setschedpolicy(&attr, SCHED_BATCH);
        if (!pthread_attr_setdetachstate(&attr,
                                         PTHREAD_CREATE_DETACHED)) {
            pthread_t thread;
            reinit_running = true;
            if (pthread_create(&thread, &attr, reinit_thread_start, NULL)) {
                reinit_running = false;
            }
        }
        pthread_attr_destroy(&attr);
    }

    if (drop_privs() != 0) {
        return -1;
    }

    // Serves the purpose of managing the last logs times read on a
    // socket connection, and as a reader lock on a range of log
    // entries.

    LastLogTimes *times = new LastLogTimes();

    // LogBuffer is the object which is responsible for holding all
    // log entries.

    logBuf = new LogBuffer(times);

    signal(SIGHUP, reinit_signal_handler);

    if (property_get_bool("logd.statistics",
                          BOOL_DEFAULT_TRUE |
                          BOOL_DEFAULT_FLAG_PERSIST |
                          BOOL_DEFAULT_FLAG_ENG |
                          BOOL_DEFAULT_FLAG_SVELTE)) {
        logBuf->enableStatistics();
    }

    // LogReader listens on /dev/socket/logdr. When a client
    // connects, log entries in the LogBuffer are written to the client.

    LogReader *reader = new LogReader(logBuf);
    if (reader->startListener()) {
        exit(1);
    }

    // LogListener listens on /dev/socket/logdw for client
    // initiated log messages. New log entries are added to LogBuffer
    // and LogReader is notified to send updates to connected clients.

    LogListener *swl = new LogListener(logBuf, reader);
    // Backlog and /proc/sys/net/unix/max_dgram_qlen set to large value
    if (swl->startListener(600)) {
        exit(1);
    }

    // Command listener listens on /dev/socket/logd for incoming logd
    // administrative commands.

    CommandListener *cl = new CommandListener(logBuf, reader, swl);
    if (cl->startListener()) {
        exit(1);
    }

    // LogAudit listens on NETLINK_AUDIT socket for selinux
    // initiated log messages. New log entries are added to LogBuffer
    // and LogReader is notified to send updates to connected clients.

    bool auditd = property_get_bool("logd.auditd",
                                    BOOL_DEFAULT_TRUE |
                                    BOOL_DEFAULT_FLAG_PERSIST);
    LogAudit *al = NULL;
    if (auditd) {
        al = new LogAudit(logBuf, reader,
                          property_get_bool("logd.auditd.dmesg",
                                            BOOL_DEFAULT_TRUE |
                                            BOOL_DEFAULT_FLAG_PERSIST)
                              ? fdDmesg
                              : -1);
    }

    LogKlog *kl = NULL;
    if (klogd) {
        kl = new LogKlog(logBuf, reader, fdDmesg, fdPmesg, al != NULL);
    }
#ifdef MTK_LOGD_DEBUG
    // add for set dumpable
    prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
#endif

    readDmesg(al, kl);

    // failure is an option ... messages are in dmesg (required by standard)

    if (kl && kl->startListener()) {
        delete kl;
    }

    if (al && al->startListener()) {
        delete al;
    }

    TEMP_FAILURE_RETRY(pause());

    exit(0);
}