/* SPDX-License-Identifier: LGPL-3.0-or-later */
/* Copyright (C) 2014 Stony Brook University
 * Copyright (C) 2020 Intel Corporation
 *                    Borys Popławski <borysp@invisiblethingslab.com>
 */

#include "api.h"
#include "libos_internal.h"
#include "libos_lock.h"
#include "libos_table.h"
#include "libos_thread.h"
#include "libos_types.h"

static void getresuid(uid_t* ruid, uid_t* euid, uid_t* suid) {
    struct libos_thread* current = get_cur_thread();
    lock(&current->lock);
    if (ruid)
        *ruid = current->uid;
    if (euid)
        *euid = current->euid;
    if (suid)
        *suid = current->suid;
    unlock(&current->lock);
}

static void getresgid(gid_t* rgid, gid_t* egid, gid_t* sgid) {
    struct libos_thread* current = get_cur_thread();
    lock(&current->lock);
    if (rgid)
        *rgid = current->gid;
    if (egid)
        *egid = current->egid;
    if (sgid)
        *sgid = current->sgid;
    unlock(&current->lock);
}

long libos_syscall_getuid(void) {
    uid_t uid;
    getresuid(&uid, NULL, NULL);
    return uid;
}

long libos_syscall_getgid(void) {
    gid_t gid;
    getresgid(&gid, NULL, NULL);
    return gid;
}

long libos_syscall_geteuid(void) {
    uid_t euid;
    getresuid(NULL, &euid, NULL);
    return euid;
}

long libos_syscall_getegid(void) {
    gid_t egid;
    getresgid(NULL, &egid, NULL);
    return egid;
}

long libos_syscall_setuid(uid_t uid) {
    int ret;
    struct libos_thread* current = get_cur_thread();

    lock(&current->lock);
    if (current->uid == 0) {
        /* if the user is root, the real UID and saved set-user-ID are also set */
        current->uid  = uid;
        current->suid = uid;
    } else if (uid != current->uid && uid != current->suid) {
        ret = -EPERM;
        goto out;
    }
    current->euid = uid;
    ret = 0;
out:
    unlock(&current->lock);
    return ret;
}

long libos_syscall_setgid(gid_t gid) {
    int ret;
    struct libos_thread* current = get_cur_thread();

    lock(&current->lock);
    if (current->uid == 0) {
        /* if the user is root, the real GID and saved set-group-ID are also set */
        current->gid  = gid;
        current->sgid = gid;
    } else if (gid != current->gid && gid != current->sgid) {
        ret = -EPERM;
        goto out;
    }
    current->egid = gid;
    ret = 0;
out:
    unlock(&current->lock);
    return ret;
}

long libos_syscall_setreuid(uid_t ruid, uid_t euid) {
    int ret = -EPERM;
    struct libos_thread* current = get_cur_thread();

    lock(&current->lock);
    if (current->euid == 0) {
        ret = 0;
    } if ((int)ruid != -1 && (int)euid != -1) {
        /* swapping or assigning only existing values ? */
        if ((ruid == current->uid || ruid == current->euid) &&
            (euid == current->uid || euid == current->euid))
            ret = 0;
    } else if ((int)ruid != -1) {
        if (ruid == current->euid)
            ret = 0;
    } else if ((int)euid != -1) {
        if (euid == current->uid || euid == current->suid)
            ret = 0;
    } else if ((int)euid == -1 && (int)ruid == -1) {
        ret = 0;
    }
    if (ret != 0)
        goto out;

    int set_suid = 0;
    if ((int)ruid != -1) {
        current->uid = ruid;
        set_suid = 1;
    }
    if ((int)euid != -1) {
        if (current->uid != euid)
            set_suid = 1;
        current->euid = euid;
    }
    if (set_suid)
        current->suid = current->euid;

    ret = 0;

out:
    unlock(&current->lock);
    return ret;
}

long libos_syscall_setresuid(uid_t ruid, uid_t euid, uid_t suid) {
    int ret;
    struct libos_thread* current = get_cur_thread();

    lock(&current->lock);
    if (current->euid != 0) {
        ret = -EPERM;
        if ((int)ruid != -1 &&
            ruid != current->uid && ruid != current->euid && ruid != current->suid)
            goto out;

        if ((int)euid != -1 &&
            euid != current->uid && euid != current->euid && euid != current->suid)
            goto out;

        if ((int)suid != -1 &&
            suid != current->uid && suid != current->euid && suid != current->suid)
            goto out;
    }
    if ((int)ruid != -1)
        current->uid = ruid;
    if ((int)euid != -1)
        current->euid = euid;
    if ((int)suid != -1)
        current->suid = suid;
    ret = 0;

out:
    unlock(&current->lock);
    return ret;
}

long libos_syscall_getresuid(uid_t* ruid, uid_t* euid, uid_t* suid) {

    if (!is_user_memory_writable(ruid, sizeof(uid_t)) ||
        !is_user_memory_writable(euid, sizeof(uid_t)) ||
        !is_user_memory_writable(suid, sizeof(uid_t)))
        return -EFAULT;

    getresuid(ruid, euid, suid);

    return 0;
}

#define NGROUPS_MAX 65536 /* # of supplemental group IDs; has to be same as host OS */

long libos_syscall_setgroups(int gidsetsize, gid_t* grouplist) {
    if (gidsetsize < 0 || (unsigned int)gidsetsize > NGROUPS_MAX)
        return -EINVAL;

    struct libos_thread* current = get_cur_thread();
    if (gidsetsize == 0) {
        free(current->groups_info.groups);
        current->groups_info.groups = NULL;
        current->groups_info.count = 0;
        return 0;
    }

    if (!is_user_memory_readable(grouplist, gidsetsize * sizeof(gid_t)))
        return -EFAULT;

    size_t groups_len = (size_t)gidsetsize;
    gid_t* groups = (gid_t*)malloc(groups_len * sizeof(*groups));
    if (!groups) {
        return -ENOMEM;
    }
    for (size_t i = 0; i < groups_len; i++) {
        groups[i] = grouplist[i];
    }

    void* old_groups = NULL;
    current->groups_info.count = groups_len;
    old_groups = current->groups_info.groups;
    current->groups_info.groups = groups;

    free(old_groups);

    return 0;
}

long libos_syscall_getgroups(int gidsetsize, gid_t* grouplist) {
    if (gidsetsize < 0)
        return -EINVAL;

    if (!is_user_memory_writable(grouplist, gidsetsize * sizeof(gid_t)))
        return -EFAULT;

    struct libos_thread* current = get_cur_thread();
    size_t ret_size = current->groups_info.count;

    if (gidsetsize) {
        if (ret_size > (size_t)gidsetsize) {
            return -EINVAL;
        }

        for (size_t i = 0; i < ret_size; i++) {
            grouplist[i] = current->groups_info.groups[i];
        }
    }

    return (int)ret_size;
}
