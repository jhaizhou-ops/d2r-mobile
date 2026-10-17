import { BackupExtensionAbility, BundleVersion } from '@kit.CoreFileKit';
import { hilog } from '@kit.PerformanceAnalysisKit';

// System backup (Settings > HUAWEI Cloud / device backup): only the saves directory is included (backup_config.json);
// the imported game data is the player's own copy and is never backed up.
export default class BackupExtension extends BackupExtensionAbility {
  async onBackup(): Promise<void> { hilog.info(0, 'XblBackup', 'backup saves'); }
  async onRestore(bundleVersion: BundleVersion): Promise<void> { hilog.info(0, 'XblBackup', 'restore from %{public}s', bundleVersion.name); }
}
