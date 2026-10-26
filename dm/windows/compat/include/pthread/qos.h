// Apple QoS 对应 Win32 线程优先级；读数来自实际优先级。
// Apple QoS maps to Win32 thread priority; readings come from the actual priority.
#pragma once
#include <pthread.h>
#include <errno.h>
#include <windows.h>
typedef enum {
    QOS_CLASS_USER_INTERACTIVE = 0x21, QOS_CLASS_USER_INITIATED = 0x19,
    QOS_CLASS_DEFAULT = 0x15, QOS_CLASS_UTILITY = 0x11,
    QOS_CLASS_BACKGROUND = 0x09, QOS_CLASS_UNSPECIFIED = 0
} qos_class_t;
static inline int pthread_set_qos_class_self_np(qos_class_t qos, int relative) {
    (void)relative;
    int p = qos == QOS_CLASS_USER_INTERACTIVE ? THREAD_PRIORITY_HIGHEST :
        qos == QOS_CLASS_USER_INITIATED ? THREAD_PRIORITY_ABOVE_NORMAL :
        qos == QOS_CLASS_BACKGROUND ? THREAD_PRIORITY_LOWEST :
        qos == QOS_CLASS_UTILITY ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_NORMAL;
    return SetThreadPriority(GetCurrentThread(), p) ? 0 : EACCES;
}
static inline int pthread_get_qos_class_np(pthread_t thread, qos_class_t *qos, int *relative) {
    int p = GetThreadPriority(pthread_gethandle(thread));
    if (p == THREAD_PRIORITY_ERROR_RETURN) return EINVAL;
    if (qos) *qos = p >= THREAD_PRIORITY_HIGHEST ? QOS_CLASS_USER_INTERACTIVE :
        p > THREAD_PRIORITY_NORMAL ? QOS_CLASS_USER_INITIATED :
        p <= THREAD_PRIORITY_LOWEST ? QOS_CLASS_BACKGROUND :
        p < THREAD_PRIORITY_NORMAL ? QOS_CLASS_UTILITY : QOS_CLASS_DEFAULT;
    if (relative) *relative = 0;
    return 0;
}
static inline qos_class_t qos_class_self(void) {
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    pthread_get_qos_class_np(pthread_self(), &qos, NULL);
    return qos;
}
