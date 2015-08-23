/*
 * Copyright (C) 2014 Matthew Finkel <Matthew.Finkel@gmail.com>
 * Copyright 2015 Daniel Martí <mvdan@mvdan.cc>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <signal.h>
#include <android/log.h>

#include "native_ioctller.h"
#include "chmacaddr.h"
#include "externals.h"

#define _GNU_SOURCE
#include <sched.h>

const static char *TAG = "chmacaddr";

struct clone_args {
    int argc;
    char **argv;
    int flags;
};

/** Confirm the string *str_mac* is formatted properly
 * Iterate over the string, verifying it is mac_hex_length
 * bytes and that it is correctly colon-separated.
 * Return -1 when the string is not mac_hex_length bytes long
 * Return -2 when it is not correctly colon-separated
 * Return 0 on success
 */
int
chmaddr_verify_string_format(const char *str_mac)
{
    int i = 0;
    if (strlen(str_mac) != mac_hex_length) {
        fprintf(stderr, "Address is not formatted properly.\n"
                        "It must be %d characters.\n", mac_hex_length);
        return -1;
    }

    for (i = 2; i < mac_hex_length; i += 3) {
        if (str_mac[i] != ':') {
            fprintf(stderr, "Address is not formatted properly.\n"
                            "Character %d must be a colon (:).\n", i);
            return -2;
        }
    }

    return 0;
}

/** Convert the string from hex-string to a byte
 * Iterate over string *strmac* in colon-separated hex
 * representation and convert it to byte representation,
 * saving the value in *mac*.
 * Return -1 if the conversion fails due to invalid format.
 * Return -2 if the string contains non-hex values (excluding ':').
 * Return 0 on success;
 *
 * Note, this function does not perform format validation. Use
 * chmaddr_verify_string_format() before calling this function.
 */
int
chmaddr_convert_hex_decode(const char *strmac, uint8_t *mac)
{
    int i = 0, octet = 0;
    char hex[3], *endptr;
    hex[2] = '\0';
    for (i = 0; i < mac_hex_length; i++) {
        long topfour;
        hex[0] = strmac[i++];
        hex[1] = strmac[i++];
        if (strmac[i] != ':' && i < mac_hex_length) {
            fprintf(stderr, "Character %d is '%c' but it should be a "
                            "colon.\n", i, strmac[i]);
            return -1;
        }
        topfour = strtoul(hex, &endptr, 16);
        if (*endptr !='\0' || topfour == ULONG_MAX)
            return -2;
        mac[octet++] = topfour & 0xFF;
    }

    return 0;
}

/** Parse and return the uid in the string
 * Parse *id* for the first value and return it as a uid_t.
 * Set *err* to 1 if the parsed value is out of range
 * and we suggest the caller invoke error handling.
 * Return a uid_t on success.
 */
uid_t
chmaddr_get_uid(const char *id, int *err)
{
    char *endptr;
    unsigned long uid = strtoul(id, &endptr, 0);
    /* Assume errno is ERANGE */
    if (*endptr != '\0' || uid == ULONG_MAX)
        *err = 1;
    return (uid_t) uid;
}

static int
accept_from_stdin(const char *iface)
{
    return 0;
}


/** Check if we currently have the ability to change our capabilities
 * Return -1 if we fail while retrieving our current capabilities
 * Return 1 if can manipulate our capabilities
 * Return 0 if we cannot.
 */
int
chmaddr_can_drop_caps(void)
{
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct have_data;
    hdr.version = _LINUX_CAPABILITY_VERSION;
    hdr.pid = getpid();
    if (capget(&hdr, &have_data)) {
        return -1;
    }
    return !!(have_data.effective & ((1<<CAP_SETPCAP) | (1<<CAP_SYS_ADMIN)));
}

/** Drop all caps we don't need later
 * We drop all capabilities except CAP_NET_ADMIN, CAP_SETGID,
 * CAP_SETUID, and CAP_SYS_ADMIN.
 *   CAP_NET_ADMIN: We need this to modify the network interface's
 *                  MAC address
 *   CAP_SETGID: We need this so we can switch gid later
 *   CAP_SETUID: We need this so we can switch uid later
 *   CAP_SYS_ADMIN: We need this so we can fork into new namespaces
 *                  (when they are available).
 * Return -1 when we fail, this includes when we fail while we're
 *   clearing our bounding set, dropping our current capabilities,
 *   and when we confirm our capabilities aren't what we set.
 * Return 0 on success.
 *
 * Note, errno is reset to 0 at the beginning of this function. You
 * may use it on failure.
 */
int
chmaddr_drop_unneeded_caps(void)
{
    int i;
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data, have_data;
    errno = 0;

    /* XXX This is borked. Come back to this. */
    /*for (i = 0; prctl(PR_CAPBSET_READ, i, 0, 0, 0) == 1; i++) {
        if (i == CAP_NET_ADMIN ||
            i == CAP_FSETID ||
            i == CAP_SETGID ||
            i == CAP_SETUID ||
            i == CAP_SETPCAP ||
            i == CAP_SYS_ADMIN)
            continue;
        if (prctl(PR_CAPBSET_DROP, i, 0, 0, 0)) {
            fprintf(stderr, "Failed to drop cap: %d, %s\n", i,
                            strerror(errno));
            return -1;
        }
    }*/

    hdr.version = _LINUX_CAPABILITY_VERSION;
    hdr.pid = 0;

    data.effective = 0;
    data.effective = 1 << CAP_NET_ADMIN;
    data.effective |= 1 << CAP_SETGID;
    data.effective |= 1 << CAP_SETUID;
    data.effective |= 1 << CAP_SETPCAP;
    /* Sadly we need to keep this in case the kernel supports
     * namespaces. It's risky, but the result is worth it, if we're
     * successful. */
    data.effective |= 1 << CAP_SYS_ADMIN;
    data.permitted = data.effective;
    data.inheritable = 0;
    if (capset(&hdr, (const cap_user_data_t)&data)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Failed to "
                            "setcap. %s\n", strerror(errno));
        return -1;
    }

    if (capget(&hdr, &have_data)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Sad. getcap "
                            "failed. %s\n", strerror(errno));
        return -1;
    }

    if (data.effective != have_data.effective) {
        __android_log_write(ANDROID_LOG_DEBUG, TAG, "Our effective "
                            "caps are not what we set! Gotta go!\n");
        return -1;
    }

    return 0;
}

/** Prevent the process (and child process) from regaining root
 * capabilities and elevating individual privileges after we
 * switch to a new, non-root, user.
 * Return -1 on failure.
 * Return 0 on success.
 *
 * Note, errno is reset to 0 at the beginning of this function. You
 * may use it on failure.
 */
int
chmaddr_lock_it_down(void)
{
    errno = 0;
    /* This is now redundant */
    if (prctl(PR_SET_KEEPCAPS, 1L, 0, 0, 0)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Failed to set "
                            "KEEPCAPS. We don't keep NET_ADMIN when "
                            "we switch users. %s\n",
                            strerror(errno));
        return -1;
    }

    /* Introduced in Linux 3.5, wait for next update */
    #if 0
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        fprintf(stderr, "Failed to set PR_SET_NO_NEW_PRIVS cap: "
                        "%s\n", strerror(errno));
        return -1;
    }
    #endif

    if (prctl(PR_SET_SECUREBITS, SECBIT_KEEP_CAPS|
                                 SECBIT_KEEP_CAPS_LOCKED |
                                 SECBIT_NO_SETUID_FIXUP |
                                 SECBIT_NO_SETUID_FIXUP_LOCKED |
                                 SECBIT_NOROOT |
                                 SECBIT_NOROOT_LOCKED)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Failed to set "
                            "PR_SET_SECUREBITS cap: %s\n",
                            strerror(errno));
        return -1;
    }

    return 0;
}

/** Called after successfully calling clone()
 * Prepare to finish_what_we_started. *args* should be a
 * struct clone_args, with argc and argv defined and sane.
 * If we successfully cloned into a new mount namespace then
 * unmount the system directories.
 * Returns the value from calling chmaddr_finish_what_we_started().
 */
int
chmaddr_clone_me(void *args)
{
    clone_args *ca = (clone_args *)args;
    int argc = ca->argc;
    const char **argv = (const char **)ca->argv;
    /* TODO find a way to test this */
    if (ca->flags & CLONE_NEWNS) {
        umount("/proc");
        umount("/sys");
        umount("/dev/pts");
        umount("/dev");
        umount("/data");
        umount("/cache");
        umount("/persist");
        umount("/system");
    }
    return chmaddr_finish_what_we_started(argc, argv);
}

static void
sa_sigaction_sigchld(int signum, siginfo_t *si, void *cxt)
{
    if (signum != SIGCHLD) {
        fprintf(stderr, "Received sig %d\n", signum);
    }
    fprintf(stderr, "Received SIGCHLD? %d\n", si->si_signo == SIGCHLD);
    fprintf(stderr, "Child was pid %d, by %u\n", si->si_pid, si->si_uid);
    if (si->si_code == CLD_EXITED) {
        fprintf(stderr, "Returned with status %d\n", si->si_status);
    } else {
        fprintf(stderr, "Killed with sig %d\n", si->si_status);
    }
}

/** Find the best invocaton of clone() which works
 * Most versions of AOSP do not ship with kernels that support
 * namepspaces. We do a little dance and call clone() until
 * it succeeds, incrementally removing different namespaces from
 * the *flags*.
 * Return -1 when we completelely fail and give up.
 * Return the pid of the resulting child process on success.
 *
 * Note, errno is reset to 0 at the beginning of this function. You
 * may use it on failure.
 */
int
chmaddr_find_valid_clone(int (*fn)(void *), void *child_stack,
                            int flags, clone_args *arg)
{
    int i;
    /* XXX Think about the this order. Perhaps we should prioritize */
    int fcombos[] =
        {0, CLONE_NEWIPC, CLONE_NEWNS, CLONE_NEWPID, CLONE_NEWUTS,
         CLONE_NEWIPC|CLONE_NEWNS, CLONE_NEWIPC|CLONE_NEWPID,
         CLONE_NEWIPC|CLONE_NEWUTS,
         CLONE_NEWNS|CLONE_NEWPID, CLONE_NEWNS|CLONE_NEWUTS,
         CLONE_NEWPID|CLONE_NEWUTS,
         CLONE_NEWIPC|CLONE_NEWNS|CLONE_NEWPID,
         CLONE_NEWIPC|CLONE_NEWPID|CLONE_NEWUTS,
         CLONE_NEWIPC|CLONE_NEWNS|CLONE_NEWUTS,
         CLONE_NEWNS|CLONE_NEWPID|CLONE_NEWUTS,
         CLONE_NEWIPC|CLONE_NEWNS|CLONE_NEWPID|CLONE_NEWUTS};
    for (i = 0; i < sizeof(fcombos); i++) {
        int pid;
        arg->flags = flags & ~fcombos[i];
        errno = 0;
        pid = clone(fn, child_stack, flags & ~fcombos[i], (void *)arg);
        if (pid != -1)
            return pid;
    }
    __android_log_print(ANDROID_LOG_DEBUG, TAG, "We failed to find "
                        "a valid set of parameters for "
                        "clone(): %s\n", strerror(errno));
    return -1;
}


/** Try to clone ourself and let our child finish
 * Setup the new stack space and prepare to call clone().
 * The parameters *argc* and *argv* are as in main() and are
 * not parsed until we finish dropping privs, after clone().
 * When we clone we try to create new ipc, mount, pid, and uts
 * namespaces. Lack of support for any of these is handled by
 * a call to chmaddr_find_valid_clone().
 * We wait until our child exits.
 * Return -1 on failure.
 * Return the status code from the child on success.
 *
 * Note, errno is reset to 0 at the beginning of this function. You
 * may use it on failure.
 */
int
chmaddr_make_it_so(int argc, const char * argv[])
{
    clone_args *ca;
    void *new_stack;
    const int stack_size = sysconf(_SC_PAGESIZE);
    int flags, i, pid;
    errno = 0;

    ca = (clone_args *)malloc(sizeof(clone_args));
    if (ca == NULL) {
        return -1;
    }

    new_stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_GROWSDOWN |
                     MAP_STACK, -1, 0);

    if (new_stack == MAP_FAILED) {
        return -1;
    }
    memset(ca, 0, sizeof(clone_args));

    ca->argc = argc;
    ca->argv = malloc(argc*sizeof(char *));
    if (ca->argv == NULL) {
        return -1;
    }

    for (i = 0; i < argc; ++i) {
        ca->argv[i] = strdup(argv[i]);
    }

    flags = 0;
    flags |= CLONE_NEWIPC;
    flags |= CLONE_NEWNS;
    flags |= CLONE_NEWPID;
    flags |= CLONE_NEWUTS;
    flags |= CLONE_UNTRACED;

    struct sigaction sa;
    memset(&sa, 0, sizeof(struct sigaction));
    sa.sa_sigaction = &sa_sigaction_sigchld;
    sa.sa_flags = SA_SIGINFO;
    if (sigaction(SIGCHLD, (const struct sigaction *)&sa, NULL)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "sigaction() "
                            "failed: %s\n", strerror(errno));
        return -1;
    }
    pid = chmaddr_find_valid_clone(&chmaddr_clone_me,
                                   new_stack + stack_size, flags, ca);
    if (pid != -1) {
        int status, exitcode;
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Waiting on %d\n",
                            pid);
        int ret = waitpid(pid, &status, __WCLONE);
        if (ret == pid) {
            if (WIFEXITED(status)) {
                __android_log_print(ANDROID_LOG_DEBUG, TAG, "pid %d "
                                    "exited with %d\n", pid,
                                    WEXITSTATUS(status));
                return WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                __android_log_print(ANDROID_LOG_DEBUG, TAG, "Child was "
                                    "terminated by sig %d\n",
                                    WTERMSIG(status));
            } else {
                __android_log_write(ANDROID_LOG_DEBUG, TAG, "Something "
                                    "else killed our baby\n");
            }
        } else {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "%d, Got "
                                "error! ECHILD? %d, EINTR? %d, EINVAL? "
                                "%d\n", ret, errno == ECHILD,
                                errno == EINTR, errno == EINVAL);
        }
        return -1;
    } else {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Failed to clone "
                            "after dropping caps: %s\n",
                            strerror(errno));
        return -1;
    }
}

#if 0 /* Bionic doesn't implement getgrnam_r() nor getpwuid_r()! */
int
chmaddr_get_group_id(const char *grpname, gid_t *gid)
{
    struct group grp;
    struct group *result;
    char *buf;
    size_t bufsize;
    int s;

    bufsize = sysconf(_SC_GETGR_R_SIZE_MAX);
    if (bufsize == -1)
        bufsize = 16384;

    buf = malloc(bufsize);
    if (buf == NULL) {
        fprintf(stderr, "Unable to allocate enough memory for  "
                        "getgrnam_r()\n");
        return -1;
    }

    s = getgrnam_r(grpname, &grp, buf, bufsize, &result);
    if (result == NULL) {
        if (s == 0) {
            fprintf(stderr, "No group found with group %s\n", grpname);
        } else {
            fprintf(stderr, "getgrnam_r lookup failed for group %s: "
                            "%s\n", grpname, strerror(s));
        }
        return -1;
    }
    *gid = grp.gr_gid;

    free(buf);
    free(result);
    return 0;
}

const gid_t*
chmaddr_get_users_groups(uid_t uid)
{
    const char *inet_group = "inet";
    gid_t gid, *gids;
    struct passwd pwd;
    struct passwd *result;
    char *buf;
    size_t bufsize;
    int s;

    if (chmaddr_get_group_id(inet_group, &gid)) {
        fprintf(stderr, "Failed to retrieve username lookup.\n");
        return NULL;
    }

    bufsize = sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufsize == -1)
        bufsize = 512;

    buf = malloc(bufsize);
    if (buf == NULL) {
        fprintf(stderr, "Unable to allocate enough memory for  "
                        "getpwnam_r()\n");
        return NULL;
    }

    s = getpwuid_r(uid, &pwd, buf, bufsize, &result);
    if (result == NULL) {
        if (s == 0) {
            fprintf(stderr, "No user found with uid %u\n", uid);
        } else {
            fprintf(stderr, "getpwuid_r lookup failed for uid %u: "
                            "%s\n", uid, strerror(s));
        }
        return NULL;
    }
    gids = malloc(sizeof(gid_t)*2);
    gids[0] = pwd.pw_gid;
    gids[1] = gid;

    free(result);
    free(buf);
    return gids;
}
#else
/** Find the group id *gid* corresponding to the group named *grpname*.
 * Look up the provided name and save the gid in *pid* if it
 * is available.
 * Return -1 if we can't find the specified group.
 * Return 0 on success.
 *
 * Note, errno is reset to 0 at the beginning of this function. You
 * may use it on failure.
 */
int
chmaddr_get_group_id(const char *grpname, gid_t *gid)
{
    struct group *grp;

    errno = 0;
    grp = getgrnam(grpname);
    if (grp == NULL) {
        if (errno == 0) {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "No group "
                                "found with groupname %s\n", grpname);
        } else {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "getpwuid_r "
                                "lookup failed for groupname %s: %s\n",
                                grpname, strerror(errno));
        }
        return -1;
    }
    *gid = grp->gr_gid;
    return 0;
}

/** Obtain the two gids we can use later to setgroups.
 * In addition to setting our group with setgid, we must also
 * be in the inet group. We lookup the the gid for inet and we
 * get the default group of the user specified by uid *uid*.
 * Return an array, size 2, of gid_t, where index 0 is the user's
 *   primary group, and index 1 is the gid of group 'inet'.
 * Return NULL on failure.
 *
 * Note, errno is reset to 0 at the beginning of this function. You
 * may use it on failure.
 */
const gid_t *
chmaddr_get_users_groups(uid_t uid)
{
    const char *inet_group = "inet";
    gid_t inet_gid, *gids;
    struct passwd *pwd;

    if (chmaddr_get_group_id(inet_group, &inet_gid)) {
        __android_log_write(ANDROID_LOG_DEBUG, TAG, "Failed to "
                            "retrieve username lookup.\n");
        return NULL;
    }

    errno = 0;
    pwd = getpwuid(uid);
    if (pwd == NULL) {
        if (errno == 0) {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "No user found "
                                "with uid %u\n", uid);
        } else {
            __android_log_print(ANDROID_LOG_DEBUG, TAG, "getpwuid_r "
                                "lookup failed for uid %u: " "%s\n",
                                uid, strerror(errno));
        }
        return NULL;
    }
    gids = malloc(sizeof(gid_t)*2);
    gids[0] = pwd->pw_gid;
    gids[1] = inet_gid;

    return gids;
}
#endif

/** Swtich to unprivileged uid
 * Switch to uid *uid*, without the ability of regaining root.
 * We also switch our primary gid the *uid*'s primary gid, and add
 * a supplementary group using chmaddr_get_users_groups(), so we have
 * access to the needed network devices. After we switch user and
 * groups, drop all remaining capabilities except CAP_NET_ADMIN (this
 * is the only capability we will need when we set the MAC address).
 * Return -1 on failure
 * Return 0 on success.
 *
 * Note, errno is reset to 0 at the beginning of this function. You
 * may use it on failure.
 */
int
chmaddr_switch_user(uid_t uid)
{
    const gid_t *groups;
    struct passwd *pwd;
    struct __user_cap_header_struct hdr;
    struct __user_cap_data_struct data, have_data;
    errno = 0;

    if (!uid) {
        __android_log_write(ANDROID_LOG_DEBUG, TAG, "Do not try "
                            "switching to uid 0.\n");
        return -1;
    }

    if (setresuid(uid, uid, uid)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "setuid to %u "
                            "failed: %s.\n", uid, strerror(errno));
        return -1;
    }
    if (getuid() != uid) {
        /* XXX We don't handle uid == -1 here,
         * but I don't think it's necessary */
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "setuid failed. "
                            "uid: %u.\n", getuid());
        return -1;
    }

    groups = chmaddr_get_users_groups(uid);
    if (groups == NULL) {
        __android_log_write(ANDROID_LOG_DEBUG, TAG, "Failed to "
                            "retrieve user's groups.\n");
        return -1;
    }

    if (setresgid(groups[0], groups[0], groups[0])) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "setgid to %u "
                            "failed: %s.\n", groups[0],
                            strerror(errno));
        return -1;
    }
    if (getgid() != groups[0]) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "setgid failed. "
                            "gid: %u.\n", getgid());
        return -1;
    }
    /* Most important, we need to re-add inet group */
    if (setgroups(2, groups)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Failed to set "
                            "supp groups groups. %s\n",
                            strerror(errno));
        return -1;
    }

    /* Drop SETUID/SETGID caps, we only need NET_ADMIN now */
    hdr.version = _LINUX_CAPABILITY_VERSION;
    hdr.pid = 0;

    data.effective = 0;
    data.effective = 1 << CAP_NET_ADMIN;
    data.permitted = data.effective;

    if (capset(&hdr, (const cap_user_data_t)&data)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Failed to "
                            "setcap - dropping SETGUID. %s\n",
                            strerror(errno));
        return -1;
    }

    if (capget(&hdr, &have_data)) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Sad. getcap "
                            "failed after dropping SETGUID. "
                            "%s\n", strerror(errno));
        return -1;
    }

    if (data.effective != have_data.effective) {
        __android_log_write(ANDROID_LOG_DEBUG, TAG, "Our effective "
                            "caps are not what we set after "
                            "dropping SETGUID! Gotta go!\n");
        return -1;
    }

    return 0;
}

/** Finish setting the new MAC address, after cloning
 * After we successfully clone, we must switch to an unprivileged
 * user and then completely drop all capabilities except
 * CAP_NET_ADMIN.
 * Next, after we successfully drop our caps, we parse the command
 * line arguments and verify the network interface name isn't too
 * long and the new MAC address is correctly formatted.
 * Return -1 if we fail somewhere along this path.
 * Return 0 if all goes well and we're successful.
 */
int
chmaddr_finish_what_we_started(int argc, const char * argv[])
{
    const char * iface;
    uint8_t mac[mac_byte_length];
    int i, err = 0;

    if (argc == 2) {
        return accept_from_stdin(argv[1]);
    }
    if (argc != 4) {
        return -1;
    }

    uid_t uid = chmaddr_get_uid(argv[3], &err);
    if (uid < 1 || err) {
        return -1;
    }

    if (chmaddr_switch_user(uid)) {
        if (errno != 0)
            fprintf(stderr, "We failed to reach a safe state.\n%s\n",
                    strerror(errno));
        return -1;
    }

    iface = argv[1];

    if (chmaddr_verify_string_format(argv[2])) {
        __android_log_print(ANDROID_LOG_DEBUG, TAG, "Address format "
                            "failed: %s, %zd.\n", argv[2],
                            strlen(argv[2]));

        return -1;
    }

    if (chmaddr_convert_hex_decode(argv[2], mac)) {
        __android_log_write(ANDROID_LOG_DEBUG, TAG,
                            "Conversion from hex to byte failed.\n");
        return -1;
    }

    if (chmaddr_confirm_caps_dropped()) {
        return -1;
    }

    int retval = nativeioc_set_mac_addr(iface, mac);
    if (retval) {
        if (errno == ENODEV) {
            fprintf(stderr, "Please temporarily enable your network "
                            "card. We could not find it.\n");
            __android_log_print(ANDROID_LOG_DEBUG, TAG,
                                "nativeioc_set_mac_addr() returned %d "
                                "with %s.\n", retval, strerror(errno));
        } else {
            fprintf(stderr, "%s.\n", strerror(errno));
        }
    }
    return retval;
}

/* Directory into which we try chrooting, as a capability test */
#define CHROOT_DIR "/mnt"

/** Try chrooting into CHROOT_DIR, so we know if we can
 * We want to know if we still have the ability to chroot (and perform
 * other privileged functions). If we successfully chroot, then we
 * know we failed somewhere and we're not in a least-privileged state.
 * If chroot-ing fails, try switching user to uid 0.
 * Return -1 if we can (or think we can) chroot or switch user.
 * Return 0 on success (where success is chroot() failing with EPERM
 *   and chmaddr_switch_user() returning -1).
 */
int
chmaddr_confirm_caps_dropped(void)
{
    /*if (prctl(PR_CAPBSET_DROP, CAP_SYS_CHROOT, 0, 0, 0) == 0) {
        fprintf(stderr, "We successfully dropped cap CAP_SYS_CHROOT "
                        "despite dropping privs and switching users\n");
        return -1;
    }*/
    if (chroot(CHROOT_DIR) == -1) {
        if (errno == EPERM) {
            return chmaddr_switch_user(1234) ? 0 : -1;
        }
        if (errno == ENOENT) {
            __android_log_write(ANDROID_LOG_DEBUG, TAG, CHROOT_DIR
                                " isn't a dir. Please report this.\n");
            /* It's likely safe to assume this would've
             * successfully failed due to nocap if CHROOT_DIR existed,
             * but if we don't deny this then we'll never know this
             * is a bad check for some devices. */
        }
    }
    fprintf(stderr, "Unable to enter secure, "
                    "least-privileged, state.\n");
    __android_log_write(ANDROID_LOG_DEBUG, TAG,
                        "We think we can still chroot, cap drop failed!");
    return -1;
}
#undef CHROOT_DIR
