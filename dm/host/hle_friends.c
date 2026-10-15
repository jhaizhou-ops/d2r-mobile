// nn::friends（好友 / 在线状态 / 邀请，排除范围）：连 friends 服务的接口一律剪成「不可用」。
// 不剪：Friend / Profile / FriendPresence 的取值函数、Friend / Profile / UserPresence 构造（只读写游戏自己的对象，不走服务）。
// 返回约定：Result 回 RES_FAIL；bool 回假；事件指针给一个已绑定内核事件、永不触发的 SystemEvent。
// nn::friends (friends / presence / invitations, out of scope): every interface that connects to the friends service is cut to "unavailable".
// Not cut: the getters of Friend / Profile / FriendPresence and the constructors of Friend / Profile / UserPresence (they only read and write the game's own objects and do not go through the service).
// Return convention: Result returns RES_FAIL; bool returns false; event pointers get a SystemEvent that is bound to a kernel event and never fires.
#include "hle.h"
#ifdef __ANDROID__
#include "../android/services_platform.h"
#endif

// 永不触发的通知事件（GetSystemEvent 类返回指针；首次在客体线程上绑定）
// Notification event that never fires (GetSystemEvent-style calls return a pointer; bound on the guest thread on first use)
static _Alignas(16) uint8_t g_ev_notify[0x100], g_ev_invite[0x100];
static void *quiet_event(uint8_t *ev) { hle_bind_system_event(ev, 0); return ev; }

HLE_FORCE("_ZN2nn7friends10InitializeEv", "离线好友策略初始化", void, h_fr_Initialize, void) {
#ifdef __ANDROID__
    dm_android_platform_setting("friends_offline_initialized", 1);
#endif
}
// AsyncContext：不发请求；完成事件立即触发、结果失败（同 hle_account.c）
// AsyncContext: no request is sent; the completion event fires immediately and the result is failure (same as hle_account.c)
HLE_FORCE("_ZN2nn7friends12AsyncContextC1Ev", "异步上下文不建会话（剪）", void, h_fr_AcCtor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_create(self, 20);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn7friends12AsyncContextD1Ev", "异步上下文无会话可关（剪）", void, h_fr_AcDtor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_destroy(self, 20);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn7friends12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "完成事件立即触发（剪）", Result, h_fr_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZNK2nn7friends12AsyncContext9GetResultEv", "异步结果恒失败（剪）", Result, h_fr_AcResult, const void *self) { (void)self; return RES_FAIL; }
// 列表 / 资料 / 游玩记录
// Lists / profiles / play history
HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendEiiRKNS0_12FriendFilterE", "好友列表为空（剪）", Result, h_fr_GetFriendList, int *count, void *out, int off, int n, const void *filt) {
    (void)out; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL;
}
HLE_FORCE("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", "资料查询失败（剪）",
          Result, h_fr_GetProfileList, void *ctx, void *out, const void *uid, const void *ids, int n) { (void)ctx; (void)out; (void)uid; (void)ids; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", "好友信息更新失败（剪）",
          Result, h_fr_UpdateFriendInfo, void *f, const void *uid, const void *ids, int n) { (void)f; (void)uid; (void)ids; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameES6_", "游玩记录不登记（剪）",
          Result, h_fr_AddPlayHistory, const void *k, const void *a, const void *b) { (void)k; (void)a; (void)b; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends29GetPlayHistoryRegistrationKeyEPNS0_26PlayHistoryRegistrationKeyERKNS_7account3UidEb", "游玩记录键不可用（剪）",
          Result, h_fr_GetPHKey, void *k, const void *uid, int local) { (void)k; (void)uid; (void)local; return RES_FAIL; }
// 通知队列
// Notification queue
HLE_FORCE("_ZN2nn7friends17NotificationQueueC1Ev", "通知队列不建会话（剪）", void, h_fr_NqCtor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_create(self, 21);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn7friends17NotificationQueueD1Ev", "通知队列无会话可关（剪）", void, h_fr_NqDtor, void *self) {
#ifdef __ANDROID__
    dm_android_platform_object_destroy(self, 21);
#else
    (void)self;
#endif
}
HLE_FORCE("_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", "通知队列初始化失败（剪）", Result, h_fr_NqInit, void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends17NotificationQueue14GetSystemEventEv", "通知事件永不触发（剪）", void *, h_fr_NqGetEv, void *self) { (void)self; return quiet_event(g_ev_notify); }
HLE_FORCE("_ZN2nn7friends17NotificationQueue3PopEPNS0_16NotificationInfoE", "通知队列恒空（剪）", Result, h_fr_NqPop, void *self, void *info) { (void)self; (void)info; return RES_FAIL; }
// 邀请
// Invitations
HLE_FORCE("_ZN2nn7friends36GetFriendInvitationNotificationEventEv", "邀请通知事件永不触发（剪）", void *, h_fr_InvEv, void) { return quiet_event(g_ev_invite); }
HLE_FORCE("_ZN2nn7friends38TryPopFriendInvitationNotificationInfoEPNS_7account3UidEPmPvm", "无邀请（剪）", int, h_fr_InvPop, void *uid, uint64_t *sz, void *buf, size_t n) {
    (void)uid; (void)sz; (void)buf; (void)n; return 0;
}
HLE_FORCE("_ZN2nn7friends21StartFriendInvitationERKNS_7account3UidEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", "邀请不可用（剪）",
          Result, h_fr_StartInv, const void *uid, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends28StartSendingFriendInvitationERKNS_7account3UidEPKNS1_23NetworkServiceAccountIdEiRKNS0_35FriendInvitationGameModeDescriptionEPKvm", "邀请不可用（剪）",
          Result, h_fr_SendInv, const void *uid, const void *ids, int n, const void *d, const void *p, size_t sz) { (void)uid; (void)ids; (void)n; (void)d; (void)p; (void)sz; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends18ShowUserDetailInfoERKNS_7account3UidERKNS1_23NetworkServiceAccountIdERKNS0_15InAppScreenNameESA_", "好友详情小程序（剪）",
          Result, h_fr_ShowDetail, const void *uid, const void *id, const void *a, const void *b) { (void)uid; (void)id; (void)a; (void)b; return RES_FAIL; }
// 在线状态
// Presence
HLE_FORCE("_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", "在线状态不可用（剪）", Result, h_fr_UpInit, void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12UserPresence14SetDescriptionEPKc", "在线状态不可用（剪）", Result, h_fr_UpDesc, void *self, const char *s) { (void)self; (void)s; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends12UserPresence6CommitEv", "在线状态不可用（剪）", Result, h_fr_UpCommit, void *self) { (void)self; return RES_FAIL; }
