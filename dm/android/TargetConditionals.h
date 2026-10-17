/* Android 垫片：共享宿主代码里的 Apple 条件编译符号（dm_cursor.c 等用 <TargetConditionals.h>）。
 * 语义与 CMake 的 TARGET_OS_IPHONE=0 一致：全部为 0，走非 Apple 分支。 */
#ifndef DM_ANDROID_TARGET_CONDITIONALS_SHIM_H
#define DM_ANDROID_TARGET_CONDITIONALS_SHIM_H
#define TARGET_OS_MAC 0
#define TARGET_OS_OSX 0
#define TARGET_OS_IPHONE 0
#define TARGET_OS_IOS 0
#define TARGET_OS_TV 0
#define TARGET_OS_SIMULATOR 0
#endif
