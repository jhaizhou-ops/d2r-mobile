import { abilityAccessCtrl, bundleManager, common, Permissions } from '@kit.AbilityKit';
import { hilog } from '@kit.PerformanceAnalysisKit';
import host from 'libxblhost.so';

// Save sync across the player's devices (same HUAWEI ID): the iOS sync core (one pack per character, later write wins,
// overwritten copies kept in files/cloud-backup) against the app's distributed files dir, which the system syncs.
// Before the game starts: download + merge. When the app goes to the background: upload only (the game may be running).
const PERM: Permissions = 'ohos.permission.DISTRIBUTED_DATASYNC';

export class SaveSync {
  static granted(): boolean {
    try {
      const info = bundleManager.getBundleInfoForSelfSync(bundleManager.BundleFlag.GET_BUNDLE_INFO_WITH_APPLICATION);
      return abilityAccessCtrl.createAtManager().checkAccessTokenSync(info.appInfo.accessTokenId, PERM) === abilityAccessCtrl.GrantStatus.PERMISSION_GRANTED;
    } catch (error) { return false; }
  }
  static async ensure(context: common.UIAbilityContext): Promise<boolean> {
    if (SaveSync.granted()) { return true; }
    try {
      const result = await abilityAccessCtrl.createAtManager().requestPermissionsFromUser(context, [PERM]);
      return result.authResults.length > 0 && result.authResults[0] === 0;
    } catch (error) { hilog.warn(0, 'XblSync', 'permission: %{public}s', String(error)); return false; }
  }
  static run(context: common.Context, allowDown: boolean): number {
    if (!SaveSync.granted()) { return -1; }
    try {
      const rc = host.cloudSync(context.filesDir + '/saves', context.distributedFilesDir + '/saves-sync',
        context.filesDir + '/cloud-backup', context.filesDir + '/cloud-state.tsv', allowDown);
      hilog.info(0, 'XblSync', 'sync allowDown=%{public}d rc=%{public}d', allowDown ? 1 : 0, rc);
      return rc;
    } catch (error) { hilog.warn(0, 'XblSync', 'sync: %{public}s', String(error)); return -1; }
  }
}
