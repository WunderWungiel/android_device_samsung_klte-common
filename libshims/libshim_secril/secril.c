/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * libsec-ril.so (List::InitList and others) calls pthread_mutexattr_settype()
 * on a stack pthread_mutexattr_t that was never pthread_mutexattr_init()ed;
 * the blob does not import pthread_mutexattr_init at all. Whether the result
 * is the intended recursive mutex then depends on stack garbage: with a stray
 * shared/protocol bit, bionic's pthread_mutex_init() returns EINVAL and the
 * zeroed (normal) mutex deadlocks on the list's nested lock. On this device
 * that hangs RIL init in PowerManager::Initialize and the radio stays
 * UNAVAILABLE.
 *
 * blob_fixup renames the blob's pthread_mutexattr_settype import to this
 * function, which initializes the attr first. That is always correct here
 * because no caller in the blob initializes it.
 */

#include <pthread.h>

int shim_secril_mutexattr_settype(pthread_mutexattr_t* attr, int type) {
    pthread_mutexattr_init(attr);
    return pthread_mutexattr_settype(attr, type);
}
