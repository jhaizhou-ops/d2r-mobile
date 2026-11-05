// 生成文件，勿手改：python3 web/scripts/generate-i18n.py；来源 web/i18n/ui_text.tsv。
// Generated; do not edit: python3 web/scripts/generate-i18n.py; source web/i18n/ui_text.tsv.
export const catalog = Object.freeze({
  "dmWebReasonsProdKeysMissing": [
    "prod.keys is missing.",
    "缺少 prod.keys。"
  ],
  "dmWebReasonsProdKeysUnreadable": [
    "prod.keys could not be read. Check local file access.",
    "无法读取 prod.keys。请检查本地文件访问权限。"
  ],
  "dmWebReasonsProdKeysTooLarge": [
    "prod.keys exceeds the 1 MiB limit.",
    "prod.keys 超过 1 MiB 限制。"
  ],
  "dmWebReasonsProdKeysEmpty": [
    "prod.keys contains no key entries.",
    "prod.keys 不包含密钥条目。"
  ],
  "dmWebReasonsProdKeysMalformed": [
    "prod.keys has an unsupported or malformed key line.",
    "prod.keys 中存在不支持或格式错误的密钥行。"
  ],
  "dmWebReasonsTitleKeyRequired": [
    "No matching title key was available from supported NSP tickets or title.keys. Add your own matching title.keys to the selected folder.",
    "支持的 NSP 票据及 title.keys 中均无匹配的标题密钥。请将你自己的匹配 title.keys 放入所选文件夹。"
  ],
  "dmWebReasonsTitleKeysMissing": [
    "title.keys is missing. An empty file is allowed when no title keys are needed.",
    "缺少 title.keys。不需要标题密钥时允许使用空文件。"
  ],
  "dmWebReasonsTitleKeysUnreadable": [
    "title.keys could not be read. Check local file access.",
    "无法读取 title.keys。请检查本地文件访问权限。"
  ],
  "dmWebReasonsTitleKeysTooLarge": [
    "title.keys exceeds the 1 MiB limit.",
    "title.keys 超过 1 MiB 限制。"
  ],
  "dmWebReasonsTitleKeysMalformed": [
    "title.keys has an unsupported or malformed key line.",
    "title.keys 中存在不支持或格式错误的密钥行。"
  ],
  "dmWebReasonsKeysRuntimeFailed": [
    "The isolated package reader could not load the key files.",
    "隔离的软件包读取器无法载入密钥文件。"
  ],
  "dmWebReasonsPackageIndexInvalid": [
    "An NSP index is unreadable, damaged, or unsupported.",
    "NSP 索引无法读取、已损坏或不受支持。"
  ],
  "dmWebReasonsTicketUnsupported": [
    "An embedded ticket format cannot be read. A matching title.keys entry can be used only when the ticket rights ID can be verified.",
    "无法读取内嵌票据格式。只有验证票据的权限 ID 后才能使用匹配的 title.keys 条目。"
  ],
  "dmWebReasonsTicketInvalid": [
    "An embedded ticket is malformed, truncated, or unreadable.",
    "内嵌票据格式错误、被截断或无法读取。"
  ],
  "dmWebReasonsTicketRightsIdMismatch": [
    "An embedded ticket name does not match its rights ID.",
    "内嵌票据名称与其权限 ID 不匹配。"
  ],
  "dmWebReasonsTicketKeyConflict": [
    "Embedded tickets contain conflicting keys for the same rights ID.",
    "内嵌票据为同一权限 ID 提供了冲突的密钥。"
  ],
  "dmWebReasonsTicketLimitExceeded": [
    "The selected packages exceed the embedded-ticket limit.",
    "所选软件包超出内嵌票据数量限制。"
  ],
  "dmWebReasonsContentReadFailed": [
    "Package content could not be read completely.",
    "无法完整读取软件包内容。"
  ],
  "dmWebReasonsContentAuthenticationFailed": [
    "Content verification failed. The keys may not match the packages, or a package may be damaged or unsupported.",
    "内容验证失败。密钥可能与软件包不匹配，或软件包已损坏或不受支持。"
  ],
  "dmWebReasonsContentMetadataInvalid": [
    "Package metadata is invalid or inconsistent.",
    "软件包元数据无效或不一致。"
  ],
  "dmWebReasonsExtractionWorkerUnavailable": [
    "The isolated package reader is unavailable.",
    "隔离的软件包读取器不可用。"
  ],
  "dmWebReasonsExtractionTransportFailed": [
    "The browser could not pass local inputs to the package reader.",
    "浏览器无法将本地输入传递给软件包读取器。"
  ],
  "dmWebReasonsExtractionProtocolFailed": [
    "The package reader returned an unexpected response.",
    "软件包读取器返回了意外响应。"
  ],
  "dmWebReasonsExtractionWorkerFailed": [
    "The isolated package reader stopped unexpectedly.",
    "隔离的软件包读取器意外停止。"
  ],
  "dmWebReasonsExtractionTimeout": [
    "Package verification exceeded its time limit.",
    "软件包验证超时。"
  ],
  "dmWebReasonsExtractionFailed": [
    "Local package verification did not complete.",
    "本地软件包验证未完成。"
  ],
  "dmWebStagesCapabilities": [
    "Checking capabilities",
    "检查功能支持"
  ],
  "dmWebStagesDiscovery": [
    "Finding local inputs",
    "查找本地输入"
  ],
  "dmWebStagesSecrets": [
    "Verifying local keys and packages",
    "验证本地密钥和软件包"
  ],
  "dmWebStagesInspection": [
    "Inspecting package identities",
    "检查软件包标识"
  ],
  "dmWebStagesSelection": [
    "Selecting package content",
    "选择软件包内容"
  ],
  "dmWebStagesExtraction": [
    "Extracting selected content",
    "提取所选内容"
  ],
  "dmWebStagesDisposal": [
    "Closing the isolated key session",
    "关闭隔离的密钥会话"
  ],
  "dmWebStagesProgram": [
    "Generating the application",
    "生成应用程序"
  ],
  "dmWebStagesAssets": [
    "Generating assets",
    "生成资源"
  ],
  "dmWebStagesShaders": [
    "Generating shaders",
    "生成着色器"
  ],
  "dmWebStagesShell": [
    "Validating the application shell",
    "验证应用程序外壳"
  ],
  "dmWebStagesValidation": [
    "Validating generated content",
    "验证生成的内容"
  ],
  "dmWebStagesAssembly": [
    "Checking the assembly",
    "检查组装结果"
  ],
  "dmWebStagesOutput": [
    "Writing the archive",
    "写入归档"
  ],
  "dmWebStagesResources": [
    "Saving game resources locally",
    "在本地保存游戏资源"
  ],
  "dmWebStagesArchive": [
    "Validating the archive",
    "验证归档"
  ],
  "dmWebStagesCleanup": [
    "Releasing temporary resources",
    "释放临时资源"
  ],
  "dmWebStagesCommit": [
    "Finalizing the archive",
    "完成归档"
  ],
  "dmWebProgresslabelsProdKeysRead": [
    "Reading prod.keys locally",
    "在本地读取 prod.keys"
  ],
  "dmWebProgresslabelsTitleKeysRead": [
    "Reading title.keys locally",
    "在本地读取 title.keys"
  ],
  "dmWebProgresslabelsKeysLoad": [
    "Loading keys into the isolated reader",
    "将密钥载入隔离的读取器"
  ],
  "dmWebProgresslabelsPackageIndex": [
    "Reading NSP indexes",
    "读取 NSP 索引"
  ],
  "dmWebProgresslabelsTicketFound": [
    "Embedded tickets found",
    "找到内嵌票据"
  ],
  "dmWebProgresslabelsTicketKeyAdded": [
    "Title keys added from tickets",
    "已从票据添加标题密钥"
  ],
  "dmWebProgresslabelsTicketKeyReused": [
    "Existing title keys reused",
    "已复用现有标题密钥"
  ],
  "dmWebProgresslabelsContentHash": [
    "Hashing package content",
    "计算软件包内容哈希"
  ],
  "dmWebProgresslabelsContentHeader": [
    "Verifying encrypted content headers",
    "验证加密内容头"
  ],
  "dmWebProgresslabelsContentMetadata": [
    "Verifying package metadata",
    "验证软件包元数据"
  ],
  "dmWebProgresslabelsRomfsRead": [
    "Reading RomFS for verification",
    "读取 RomFS 以进行验证"
  ],
  "dmWebProgresslabelsRomfsBase": [
    "Verifying base RomFS",
    "验证基础 RomFS"
  ],
  "dmWebProgresslabelsRomfsMerged": [
    "Verifying merged update RomFS",
    "验证合并更新后的 RomFS"
  ],
  "dmWebStageState": [
    "%s: %s",
    "%s：%s"
  ],
  "dmWebRunning": [
    "running",
    "进行中"
  ],
  "dmWebComplete": [
    "complete",
    "完成"
  ],
  "dmWebFailed": [
    "failed",
    "失败"
  ],
  "dmWebCancelled": [
    "Cancelled.",
    "已取消。"
  ],
  "dmWebLocalPackaging": [
    "Local packaging",
    "本地打包"
  ],
  "dmWebFailureSummary": [
    "%s failed%s. %sNo completed IPA is available.",
    "%s失败%s。%s没有可用的完整 IPA。"
  ],
  "dmWebOutputPossible": [
    " Resources may have been saved in the data folder inside the selected folder.",
    " 资源可能已保存到所选文件夹内的 data 文件夹。"
  ],
  "dmWebOutputNone": [
    " No resources were exported by this attempt.",
    " 本次尝试未导出资源。"
  ],
  "dmWebOutputComplete": [
    " Complete game resources remain in %s inside the selected folder, but no completed IPA is available.",
    " 完整游戏资源保留在所选文件夹内的 %s 中，但没有可用的完整 IPA。"
  ],
  "dmWebOutputRemoved": [
    " The incomplete resource export was removed; no resource folder from this attempt remains.",
    " 已删除未完成的资源导出；本次尝试未留下资源文件夹。"
  ],
  "dmWebOutputIncomplete": [
    " Incomplete resources may remain in %s inside the selected folder. Do not use this as a finished resource set. Check the folder before retrying.%s",
    " 未完成的资源可能保留在所选文件夹内的 %s 中。不要将其用作完整资源集。重试前请检查该文件夹。%s"
  ],
  "dmWebMarkerPresent": [
    " An incomplete-export warning marker remains.",
    " 未完成导出的警告标记仍然存在。"
  ],
  "dmWebMarkerUnknown": [
    " An incomplete-export warning marker could not be confirmed.",
    " 无法确认未完成导出的警告标记。"
  ],
  "dmWebFailureRetry": [
    " Reload to try again. Keep key files on this device; the activity log contains no keys or filenames.",
    " 请重新载入后重试。密钥文件应保留在本设备上；活动日志不包含密钥或文件名。"
  ],
  "dmWebRomfsVerified": [
    "%s: %s%% (%s MB verified)",
    "%s：%s%%（已验证 %s MB）"
  ],
  "dmWebLoading": [
    "Loading application files…",
    "正在载入应用文件…"
  ],
  "dmWebUnsupported": [
    "This build is not ready, or this browser is unsupported. Use desktop Chrome or Edge.",
    "此版本尚未就绪，或当前浏览器不受支持。请使用桌面版 Chrome 或 Edge。"
  ],
  "dmWebLoadingVerified": [
    "Loading verified application files",
    "正在载入已验证的应用文件"
  ],
  "dmWebDownloading": [
    "Downloading application files",
    "正在下载应用文件"
  ],
  "dmWebPreparing": [
    "Preparing isolated application runtimes",
    "正在准备隔离的应用运行时"
  ],
  "dmWebReady": [
    "Ready. Files stay on this device.",
    "已就绪。文件始终保留在本设备上。"
  ],
  "dmWebApplicationReady": [
    "Application ready",
    "应用已就绪"
  ],
  "dmWebSetupFailed": [
    "Application setup failed before local inputs were opened",
    "应用准备失败，尚未打开本地输入"
  ],
  "dmWebWaitingFolder": [
    "Waiting for folder selection…",
    "等待选择文件夹…"
  ],
  "dmWebCancelledReload": [
    "Cancelled. Reload to start again.",
    "已取消。请重新载入以再次开始。"
  ],
  "dmWebFolderFailed": [
    "Folder check failed; check the status message",
    "文件夹检查失败；请查看状态消息"
  ],
  "dmWebMissingInput": [
    "Put NSP/XCI packages and prod.keys directly inside this folder, not in a subfolder.",
    "请将 NSP/XCI 软件包和 prod.keys 直接放入此文件夹，而非子文件夹。"
  ],
  "dmWebFolderPermission": [
    "Choose the folder again and allow access to read inputs and save the resource subfolder.",
    "请重新选择文件夹，并允许读取输入及保存资源子文件夹。"
  ],
  "dmWebFolderUnreadable": [
    "The folder could not be read. Check access and choose it again.",
    "无法读取该文件夹。请检查访问权限并重新选择。"
  ],
  "dmWebStarting": [
    "Starting IPA generation…",
    "正在开始生成 IPA…"
  ],
  "dmWebGenerationStarted": [
    "Generation started; private inputs remain local",
    "已开始生成；私密输入仍保留在本地"
  ],
  "dmWebFastMode": [
    "Fast mode: full RomFS scan will be skipped after package hashes match",
    "快速模式：软件包哈希匹配后将跳过完整 RomFS 扫描"
  ],
  "dmWebVerifyHashOption": [
    "Verify full content hashes (slower)",
    "验证完整内容哈希（较慢）"
  ],
  "dmWebVerifyHashHelp": [
    "Off by default for faster generation; signed headers and package structure are still checked. Turn on to hash all package content.",
    "默认关闭以加快生成；仍会检查签名头部和软件包结构。开启后会读取并验证所有软件包内容。"
  ],
  "dmWebSkipHashActive": [
    "Fast mode: skipping full content hashes and RomFS scan; signed headers and structure are still checked",
    "快速模式：跳过完整内容哈希及 RomFS 扫描；仍会检查签名头部和结构"
  ],
  "dmWebProgresslabelsExefsOpen": [
    "Opening selected executable content",
    "打开选定的可执行内容"
  ],
  "dmWebProgresslabelsExefsRead": [
    "Reading executable modules",
    "读取可执行模块"
  ],
  "dmWebProgresslabelsRomfsOpen": [
    "Opening selected RomFS",
    "打开选定的 RomFS"
  ],
  "dmWebProgresslabelsRomfsIndex": [
    "Indexing RomFS files",
    "索引 RomFS 文件"
  ],
  "dmWebProgresslabelsLogicalIndex": [
    "Indexing game assets",
    "索引游戏资源"
  ],
  "dmWebUnsignedReady": [
    "Unsigned IPA ready; resources saved locally",
    "未签名 IPA 已就绪；资源已保存到本地"
  ],
  "dmWebGenerating": [
    "Generating IPA…",
    "正在生成 IPA…"
  ],
  "dmWebGenerate": [
    "Generate IPA",
    "生成 IPA"
  ],
  "dmWebCancellingWait": [
    "Cancelling… Waiting for the current operation to stop.",
    "正在取消… 等待当前操作停止。"
  ],
  "dmWebCancellationRequested": [
    "Cancellation requested",
    "已请求取消"
  ],
  "dmWebCancelling": [
    "Cancelling…",
    "正在取消…"
  ],
  "dmWebCancel": [
    "Cancel",
    "取消"
  ],
  "dmWebStopping": [
    "Stopping generation",
    "正在停止生成"
  ],
  "dmWebInProgress": [
    "Generation in progress",
    "正在生成"
  ],
  "dmWebStoppingAria": [
    "Stopping IPA generation",
    "正在停止生成 IPA"
  ],
  "dmWebProgressAria": [
    "IPA generation in progress",
    "正在生成 IPA"
  ],
  "dmWebWaitCleanup": [
    "Waiting for cancellation and cleanup to finish.",
    "等待取消和清理完成。"
  ],
  "dmWebLargePackages": [
    "Large packages can take several minutes to verify, even when the activity log is quiet.",
    "大型软件包验证可能需要数分钟，即使活动日志暂时没有新内容也属正常。"
  ],
  "dmWebCopied": [
    "Log copied.",
    "已复制日志。"
  ],
  "dmWebCopiedNew": [
    "Log copied. New entries have arrived; copy again for the latest log.",
    "已复制日志。有新记录到达；请再次复制以获取最新日志。"
  ],
  "dmWebCopyFailed": [
    "Copy failed. Select the log text and copy it manually.",
    "复制失败。请选中日志文字并手动复制。"
  ],
  "dmWebReloadRetry": [
    "Reload and retry",
    "重新载入并重试"
  ],
  "dmWebReload": [
    "Reload to start again",
    "重新载入以再次开始"
  ],
  "dmWebTitle": [
    "D2R Mobile",
    "D2R Mobile"
  ],
  "dmWebChoose": [
    "Choose folder",
    "选择文件夹"
  ],
  "dmWebPackages": [
    "Packages to use",
    "要使用的软件包"
  ],
  "dmWebDownload": [
    "Download IPA",
    "下载 IPA"
  ],
  "dmWebCopy": [
    "Copy log",
    "复制日志"
  ],
  "dmWebInstructions": [
    "Put your NSP/XCI packages and prod.keys in one folder. Some packages also need title.keys.",
    "将 NSP/XCI 软件包和 prod.keys 放在同一文件夹。部分软件包还需要 title.keys。"
  ],
  "dmWebSelectionHelp": [
    "Base and updates are preselected when filenames identify one base and its updates; otherwise all packages are selected. Review your selection. Include DLC for Warlock content; excluded files stay untouched.",
    "当文件名能识别出一个基础包及其更新时，会预先选中这些文件；否则将选中全部软件包。请检查所选内容。要使用术士内容，请包含 DLC；未选中的文件不会被改动。"
  ],
  "dmWebDownloadHelp": [
    "Your browser controls where the IPA is saved. If the download did not start, use this button.",
    "IPA 的保存位置由浏览器决定。如果下载未开始，请使用此按钮。"
  ],
  "dmWebLogHelp": [
    "Stages, counts, error codes and filtered shader diagnostics. No key contents or selected filenames. Kept in this tab, not uploaded. Times use your browser's local time.",
    "记录阶段、计数、错误代码和已过滤的着色器诊断，不含密钥内容或所选文件名。日志保留在当前标签页，不会上传。时间采用浏览器本地时间。"
  ],
  "dmWebPrivacy": [
    "The IPA downloads normally. Large game resources stream to a new subfolder inside your selected folder. Existing NSPs and keys are not changed. Files never leave this device.",
    "IPA 将正常下载。大型游戏资源会流式写入所选文件夹内的新子文件夹。现有 NSP 和密钥不会被改动。文件不会离开本设备。"
  ],
  "dmWebFound": [
    "found",
    "已找到"
  ],
  "dmWebMissing": [
    "missing",
    "缺失"
  ],
  "dmWebFoundOptional": [
    "found (optional)",
    "已找到（可选）"
  ],
  "dmWebNotProvided": [
    "not provided (optional)",
    "未提供（可选）"
  ],
  "dmWebProvided": [
    "provided",
    "已提供"
  ],
  "dmWebFolderSummary": [
    "%s package(s); prod.keys %s; title.keys %s.",
    "%s 个软件包；prod.keys %s；title.keys %s。"
  ],
  "dmWebManifest": [
    "Runtime manifest: %s",
    "运行时清单：%s"
  ],
  "dmWebSetupError": [
    "Application setup failed: %s. Reload to retry. No game files have been opened.",
    "应用准备失败：%s。请重新载入后重试。尚未打开任何游戏文件。"
  ],
  "dmWebChecking": [
    "Checking %s…",
    "正在检查 %s…"
  ],
  "dmWebCheckingProgress": [
    "Checking %s… %s",
    "正在检查 %s… %s"
  ],
  "dmWebFolderChecked": [
    "Folder checked: %s packages; prod.keys found; title.keys %s",
    "文件夹检查完成：%s 个软件包；已找到 prod.keys；title.keys %s"
  ],
  "dmWebReviewSelection": [
    "%s: %s Review the package selection before generating.",
    "%s：%s 请在生成前检查所选软件包。"
  ],
  "dmWebDownloadReady": [
    "IPA ready to download. Resources saved in %s/%s. Copy that resource folder into the app’s Files folder after signing and installing the IPA. Game compatibility is not yet verified.",
    "IPA 已可下载。资源已保存到 %s/%s。签名并安装 IPA 后，请将该资源文件夹复制到应用的“文件”文件夹中。游戏兼容性尚未验证。"
  ],
  "dmWebSelectedCount": [
    "%s of %s package(s) selected.",
    "已选择 %s 个软件包，共 %s 个。"
  ],
  "dmWebSelectOne": [
    " Select at least one package to generate an IPA.",
    " 请至少选择一个软件包以生成 IPA。"
  ],
  "dmWebBase": [
    "Base",
    "基础"
  ],
  "dmWebMerged": [
    "Merged update",
    "合并更新"
  ],
  "dmWebVerification": [
    "%s RomFS verification",
    "%s RomFS 验证"
  ],
  "dmWebVerificationPercent": [
    "%s RomFS: %s%% verified.",
    "%s RomFS：已验证 %s%%。"
  ],
  "dmWebLogCount": [
    "Activity log (%s)",
    "活动日志（%s）"
  ],
  "dmWebLog": [
    "Activity log",
    "活动日志"
  ],
  "dmWebFilesLoaded": [
    "Application files loaded",
    "已载入应用文件"
  ],
  "dmWebFilesReady": [
    "%s / %s · %s / %s files ready",
    "%s / %s · %s / %s 个文件已就绪"
  ],
  "dmWebDownloaded": [
    "%s downloaded",
    "已下载 %s"
  ],
  "dmWebCached": [
    " · %s from cache",
    " · %s 来自缓存"
  ],
  "dmWebMegabytes": [
    "%s MB",
    "%s MB"
  ],
  "dmWebProgressCount": [
    ": %s",
    "：%s"
  ],
  "dmWebProgressFraction": [
    " (%s/%s)",
    "（%s/%s）"
  ],
  "dmWebCompilerWorkers": [
    " · compiler workers: %s",
    " · 编译工作线程：%s"
  ],
  "dmWebProgressSize": [
    "; %s MB",
    "；%s MB"
  ],
  "dmWebResourceProgress": [
    "Saving game resources: %s/%s files; %s/%s MB",
    "正在保存游戏资源：%s/%s 个文件；%s/%s MB"
  ],
  "dmWebCompatibility": [
    "IPA target: iOS 26+. Default output requires BC texture support. Signing is separate.",
    "IPA 目标：iOS 26 或更新版本；默认需要 GPU 支持 BC 纹理。签名需另行完成。"
  ],
  "dmWebTarget": [
    "IPA target",
    "IPA 目标"
  ],
  "dmWebTarget26": [
    "iOS 26+",
    "iOS 26 或更新版本"
  ],
  "dmWebTarget18": [
    "iOS 18+",
    "iOS 18 或更新版本"
  ],
  "dmWebTargetReload": [
    "Changes apply when you generate; your selected folder is kept.",
    "更改将在生成时生效；已选文件夹会保留。"
  ],
  "dmWebCompatibility18": [
    "IPA target: iOS 18+. Default output requires BC texture support. Not a guarantee for every device. Signing is separate.",
    "IPA 目标：iOS 18 或更新版本；默认需要 GPU 支持 BC 纹理。不代表所有设备均可运行；签名需另行完成。"
  ],
  "dmWebLegacyOption": [
    "Older-GPU option (experimental)",
    "较旧 GPU 选项（实验性）"
  ],
  "dmWebLegacyHelp": [
    "Exports a separate Legacy Device Pack. Device compatibility is not verified.",
    "将另行导出 Legacy Device Pack。设备兼容性尚未验证。"
  ],
  "dmWebLegacyPreparing": [
    "Legacy Device Pack: preparing; exported separately from the IPA.",
    "Legacy Device Pack：正在准备；与 IPA 分开导出。"
  ],
  "dmWebLegacySaving": [
    "Legacy Device Pack: saving %s / %s.",
    "Legacy Device Pack：正在保存 %s / %s。"
  ],
  "dmWebLegacyComplete": [
    "Legacy Device Pack saved to %s. Copy it alongside the game resources into the app’s Files folder; this does not mean the IPA is complete.",
    "Legacy Device Pack 已保存到 %s。请与游戏资源一同复制到应用的文件夹；这不代表 IPA 已完成。"
  ],
  "dmWebLegacyIncomplete": [
    "Legacy Device Pack is not complete. Do not use a partial folder; reload to retry.",
    "Legacy Device Pack 未完成。请勿使用不完整的文件夹；重新加载后重试。"
  ],
  "dmWebLegacyConverting": [
    "Legacy Device Pack: processed %d blocks; %s MB of work (multiple passes).",
    "Legacy Device Pack：已处理 %d 个块；处理工作量 %s MB（含多轮处理）。"
  ],
  "dmWebSkipResources": [
    "Skip resource export (already on device)",
    "跳过资源导出（设备上已有）"
  ],
  "dmWebSkipResourcesHelp": [
    "Only for the same game, update and DLC. Existing device files are not verified.",
    "仅适用于相同的游戏、更新和 DLC。不会验证设备上已有的文件。"
  ],
  "dmWebSkipKeepsLegacy": [
    "Legacy Device Pack still exports.",
    "仍会导出 Legacy Device Pack。"
  ],
  "dmWebResourcesSkipped": [
    "Game resource export skipped; device files were not checked.",
    "已跳过游戏资源导出；未检查设备上的文件。"
  ],
  "dmWebStorageEstimate": [
    "Estimated browser staging space available: %s. Not destination disk space or a reservation.",
    "浏览器暂存空间估计：可用 %s；不是目标磁盘空间，也不是预留空间。"
  ],
  "dmWebStorageUnknown": [
    "Browser staging space estimate unavailable; you can continue. Destination disk space was not checked.",
    "浏览器暂存空间估计不可用；仍可继续。目标磁盘空间未检查。"
  ],
  "dmWebAstcPreset": [
    "ASTC encoding preset",
    "ASTC 编码预设"
  ],
  "dmWebAstcThorough": [
    "Thorough (default)",
    "精细（默认）"
  ],
  "dmWebAstcFast": [
    "Fast",
    "快速"
  ],
  "dmWebAstcPresetHelp": [
    "Fast takes less processing time but may reduce image quality. Changes apply when you generate.",
    "快速模式耗时较短，但可能降低图像质量。更改将在生成时生效。"
  ],
  "dmWebDownloadPreparationFailed": [
    "The IPA was generated, but its download link could not be prepared. Completed resource exports are unchanged; retry the download without converting again.",
    "IPA 已生成，但无法准备下载链接。已完成的资源导出不受影响；请重试下载，无需重新转换。"
  ],
  "dmWebRetryDownloadPreparation": [
    "Retry IPA download preparation",
    "重试准备 IPA 下载"
  ],
  "dmWebCommittedCleanupWarning": [
    "The unsigned IPA is complete and available to save, but cleanup failed. Save the IPA, then reload before another attempt.",
    "未签名 IPA 已完成并可保存，但清理失败。请先保存 IPA，再重新载入后进行下一次尝试。"
  ],
  "dmWebResourcesSaved": [
    "Game resources saved in %s inside the selected folder.",
    "游戏资源已保存到所选文件夹内的 %s 中。"
  ],
  "dmWebSigningOption": [
    "Optional: sign the IPA locally",
    "可选：在本地签名 IPA"
  ],
  "dmWebSigningHelp": [
    "Unsigned IPA by default. Enable local signing before generating. Your P12 and password are processed locally, not uploaded.",
    "默认输出未签名 IPA。可在生成前启用本地签名；P12 与密码仅在本地处理，不会上传。"
  ],
  "dmWebSigningCredentials": [
    "Local signing credentials",
    "本地签名凭据"
  ],
  "dmWebSigningIdentity": [
    "P12 certificate",
    "P12 证书"
  ],
  "dmWebSigningPassword": [
    "Identity password (may be empty)",
    "证书密码（可留空）"
  ],
  "dmWebSigningProfile": [
    "Provisioning profile (.mobileprovision)",
    "描述文件（.mobileprovision）"
  ],
  "dmWebSigningWorking": [
    "Unsigned IPA retained; signing and checking the archive locally.",
    "未签名 IPA 已保留；正在本地签名并检查归档。"
  ],
  "dmWebSigningComplete": [
    "Signed IPA created and ready to download; device installation or execution is not guaranteed.",
    "已签名 IPA 已创建并可下载；这不保证设备可安装或运行。"
  ],
  "dmWebSigningFailed": [
    "Signing failed. The unsigned IPA and exported resources remain available; select credentials again to retry signing without reconverting.",
    "签名失败。未签名 IPA 和已导出的资源仍可使用；可重新选择凭据后重试签名，无需重新转换。"
  ],
  "dmWebSigningCancelled": [
    "Signing cancelled. The unsigned IPA and exported resources remain available; select credentials again to retry signing.",
    "签名已取消。未签名 IPA 和已导出的资源仍可使用；可重新选择凭据后重试签名。"
  ],
  "dmWebSignedDownload": [
    "Download signed IPA",
    "下载已签名 IPA"
  ],
  "dmWebSignedHelp": [
    "Device eligibility, installation and game compatibility still require device checks.",
    "设备资格、安装和游戏兼容性仍需在设备上检查。"
  ],
  "dmWebCleanupWarning": [
    "Cleanup failed. Save any available IPA before reloading.",
    "清理失败。请保存可用的 IPA 后再重新载入。"
  ],
  "dmWebSigningProfileMismatch": [
    "The identity or profile is mismatched or expired, or this signing tool does not support the profile. Unsigned IPA retained; select compatible credentials to retry signing.",
    "证书或描述文件不匹配、已过期，或此签名工具不支持该描述文件。未签名 IPA 已保留；请选择兼容凭据后重试签名。"
  ],
  "dmWebRetrySigning": [
    "Retry signing",
    "重试签名"
  ],
  "dmWebShaderFailureContext": [
    "Shader details: %s",
    "着色器诊断：%s"
  ],
  "dmWebShaderSourceDigest": [
    "Source digest: %s",
    "源摘要：%s"
  ],
  "dmWebInputMode": [
    "Game input",
    "游戏输入"
  ],
  "dmWebInputPackages": [
    "NSP / XCI packages",
    "NSP / XCI 软件包"
  ],
  "dmWebInputExtracted": [
    "Extracted ExeFS / RomFS",
    "已提取的 ExeFS / RomFS"
  ],
  "dmWebExtractedInstructions": [
    "Merged ExeFS and RomFS detected. No keys needed. Base/update only; expansion input is unsupported and DLC metadata is not authenticated.",
    "已识别合并的 ExeFS 和 RomFS，无需密钥。仅支持基础游戏与更新；不支持扩展内容，DLC 元数据未经认证。"
  ],
  "dmWebExtractedSummary": [
    "%s extracted file(s) found; no keys needed.",
    "已找到 %s 个提取文件；无需密钥。"
  ],
  "dmWebExtractedChecked": [
    "Merged base/update folder selected; DLC metadata is not authenticated.",
    "已选择合并的基础游戏与更新文件夹；DLC 元数据未经认证。"
  ],
  "dmWebExtractedReady": [
    "%s: %s Extracted files found.",
    "%s：%s 已找到提取后的文件。"
  ],
  "dmWebExtractedMissingInput": [
    "Choose a folder containing ExeFS/main, ExeFS/sdk, and RomFS/data/build_manifest.psv.",
    "请选择包含 ExeFS/main、ExeFS/sdk 和 RomFS/data/build_manifest.psv 的文件夹。"
  ],
  "dmWebExtractedInvalidName": [
    "The extracted folder contains invalid or duplicate paths. Check the folder and try again.",
    "已提取文件夹包含无效或重复路径。请检查文件夹后重试。"
  ],
  "dmWebExtractedLimit": [
    "The extracted folder exceeds file-count or path-length limits.",
    "已提取文件夹超出文件数量或路径长度限制。"
  ],
  "dmWebSupportedVersions": [
    "Game version: %s only. Other versions are not supported.",
    "游戏版本：仅 %s。其他版本暂不支持。"
  ],
  "dmWebProgramUnsupportedVersion": [
    "This game executable version is not supported. Use a version listed above.",
    "此游戏程序版本尚不支持。请使用上方列出的版本。"
  ],
  "dmWebProgramModuleMismatch": [
    "The ExeFS modules are from different releases. Use a complete set from one release.",
    "ExeFS 模块来自不同游戏版本。请使用同一版本的完整文件。"
  ],
  "dmWebProgramShellMismatch": [
    "This game release is not compatible with the selected iOS target.",
    "此游戏版本与所选 iOS 目标不兼容。"
  ],
  "dmWebProgramInvalidModules": [
    "ExeFS is missing required modules or contains extra modules.",
    "ExeFS 缺少必需模块或包含额外模块。"
  ],
  "dmWebProgramDigestMismatch": [
    "The game executable does not match a supported release. Check the source files.",
    "游戏程序与支持版本不匹配。请检查源文件。"
  ],
  "dmWebProgramBuildIdMismatch": [
    "The game executable build ID does not match a supported release.",
    "游戏程序构建标识与支持版本不匹配。"
  ],
  "dmWebCustomizeApp": [
    "Customize app (optional)",
    "自定义应用（可选）"
  ],
  "dmWebCustomHelp": [
    "The selected shell’s defaults are shown. Edit any value; clear it or reset to restore the default. Files and settings stay on this device and are not uploaded.",
    "已显示所选外壳的默认值。可直接编辑；清空或重置以恢复默认值。文件和设置仅在本设备使用，不上传。"
  ],
  "dmWebCustomName": [
    "App name",
    "应用名称"
  ],
  "dmWebCustomBundle": [
    "Bundle ID",
    "应用标识符"
  ],
  "dmWebCustomBundleHelp": [
    "Your signing profile must match the new bundle ID. Changing it creates a separate app identity.",
    "签名描述文件必须匹配新的应用标识符。更改标识符会被视为另一个应用。"
  ],
  "dmWebCustomVersion": [
    "Version (e.g. 1.0.0)",
    "版本号（如 1.0.0）"
  ],
  "dmWebCustomBuild": [
    "Build number (e.g. 1 or 1.0)",
    "构建号（如 1 或 1.0）"
  ],
  "dmWebCustomIcon": [
    "App icon (PNG)",
    "应用图标（PNG）"
  ],
  "dmWebCustomIconHelp": [
    "Opaque square PNG, 256–1024 px, up to 4 MiB; 8-bit RGB/RGBA, not interlaced. Custom icons may not appear in every iOS view; appearance is unverified.",
    "不透明正方形 PNG，256–1024 像素，最多 4 MiB；8 位 RGB/RGBA，非交错。自定义图标可能不会显示在所有 iOS 界面，实际效果尚未验证。"
  ],
  "dmWebCustomRemoveIcon": [
    "Remove custom icon",
    "移除自定义图标"
  ],
  "dmWebCustomNameInvalid": [
    "Use an app name of up to 80 characters without control characters or leading/trailing spaces.",
    "应用名称最多 80 个字符，不能包含控制字符或首尾空格。"
  ],
  "dmWebCustomBundleInvalid": [
    "Use a bundle ID of up to 255 characters with dot-separated letters, digits, or hyphens, such as com.example.game.",
    "应用标识符最多 255 个字符，仅使用由句点分隔的字母、数字或连字符，如 com.example.game。"
  ],
  "dmWebCustomVersionInvalid": [
    "Use three dot-separated version numbers, such as 1.0.0, without leading zeros.",
    "版本号必须是三个由句点分隔的数字，如 1.0.0，不能包含前导零。"
  ],
  "dmWebCustomBuildInvalid": [
    "Use a build number starting with 1–9999, optionally followed by up to two numbers from 0–99, without leading zeros.",
    "构建号首段为 1–9999，可再加最多两段 0–99 的数字，不能包含前导零。"
  ],
  "dmWebCustomIconInvalid": [
    "Choose a valid opaque square PNG, 256–1024 px, up to 4 MiB, with 8-bit RGB/RGBA and no interlacing.",
    "请选择有效的不透明正方形 PNG，256–1024 像素，最多 4 MiB，8 位 RGB/RGBA 且非交错。"
  ],
  "dmWebCustomInvalid": [
    "These app customizations could not be applied. Check the settings and try again.",
    "无法应用这些自定义设置。请检查设置后重试。"
  ],
  "dmWebCustomChecking": [
    "Checking app settings…",
    "正在检查应用设置…"
  ],
  "dmWebCustomCorrect": [
    "Correct the app customization and try again.",
    "请更正应用自定义设置后重试。"
  ],
  "dmWebCustomOriginalRetained": [
    "Customization was not applied. The retained unsigned IPA has the original app name, bundle ID, version, and icon; it was not automatically signed.",
    "自定义未应用。保留的未签名 IPA 使用原始应用名称、标识符、版本和图标；未自动签名。"
  ],
  "dmWebCustomOriginalDownload": [
    "Download original unsigned IPA",
    "下载原始未签名 IPA"
  ],
  "dmWebStagesCustomization": [
    "Customizing app",
    "自定义应用"
  ],
  "dmWebExtractedInputInvalid": [
    "The extracted input could not be validated. Check the merged ExeFS and RomFS folders.",
    "无法验证已提取的输入。请检查已合并的 ExeFS 和 RomFS 文件夹。"
  ],
  "dmWebExtractedDlcUnsupported": [
    "Expansion input is not supported for extracted folders. Use merged base/update files or switch to NSP/XCI input.",
    "已提取文件夹不支持扩展内容输入。请使用已合并的基础游戏与更新文件，或改选 NSP/XCI 输入。"
  ],
  "dmWebExtractedResourceLayout": [
    "The extracted resource layout is not supported. Use already-merged base/update resources.",
    "此提取文件夹的资源布局不受支持。请使用已合并的基础游戏与更新资源。"
  ],
  "dmWebExtractedSkipResourcesHelp": [
    "Skip only if the device already has resources from this exact merged folder. Existing device files are not verified.",
    "仅在设备上已有此合并文件夹对应的资源时跳过。不会验证设备上已有的文件。"
  ],
  "dmWebAutoInstructions": [
    "Choose your game folder: NSP/XCI with prod.keys, or already-merged ExeFS/RomFS. The format is detected automatically.",
    "选择游戏文件夹，自动识别 NSP/XCI（需 prod.keys）或已合并的 ExeFS/RomFS。"
  ],
  "dmWebOutputCompatibility": [
    "iOS 26+ by default; iOS 18+ compatibility in Advanced. Selected: %s. %s",
    "默认 iOS 26+；高级选项中可选 iOS 18+ 兼容模式。当前选择：%s。%s"
  ],
  "dmWebBcTextures": [
    "BC texture support required.",
    "需要 GPU 支持 BC 纹理。"
  ],
  "dmWebLegacyTextures": [
    "Experimental older-GPU textures; device compatibility unverified.",
    "实验性旧 GPU 纹理；设备兼容性未经验证。"
  ],
  "dmWebOptionsBeforeFolder": [
    "Change the options below before generating, without reloading or reselecting your folder.",
    "生成前可随时调整下方选项，无需重新载入或重新选择文件夹。"
  ],
  "dmWebAdvancedOptions": [
    "Advanced",
    "高级选项"
  ],
  "dmWebSigningOptions": [
    "Local signing (optional)",
    "本地签名（可选）"
  ],
  "dmWebDetectingInput": [
    "Detecting game files…",
    "正在识别游戏文件…"
  ],
  "dmWebIncludedPackages": [
    "All %s packages included",
    "包含全部 %s 个软件包"
  ],
  "dmWebPackagesReady": [
    "%s: %s All packages included.",
    "%s：%s 全部软件包已包含。"
  ],
  "dmWebSigningApplicationIdTooLong": [
    "The profile prefix plus bundle ID exceeds this signer's 255-character limit. Unsigned IPA retained; use it with another signer, or reload and generate with a shorter bundle ID.",
    "描述文件前缀加应用标识超过此签名工具的 255 字符上限。未签名 IPA 已保留；请使用其他签名工具，或重新载入并以较短应用标识生成。"
  ],
  "dmWebCustomDefaultIcon": [
    "Using the selected shell’s default icon. Choose a PNG to replace it.",
    "使用所选外壳的默认图标。选择 PNG 以替换。"
  ],
  "dmWebCustomReset": [
    "Reset to shell defaults",
    "恢复外壳默认值"
  ],
  "dmWebGuideTitle": [
    "Before you start",
    "开始前的准备"
  ],
  "dmWebGuidePrepare": [
    "Prepare one game folder",
    "准备一个游戏文件夹"
  ],
  "dmWebGuidePackages": [
    "Put supported base and compatible update NSP/XCI files with prod.keys at the root of one folder. Add title.keys only if a matching external title key is needed; compatible known DLC is optional.",
    "将支持的基础游戏与兼容更新 NSP/XCI、prod.keys 放在同一文件夹根目录。仅在需要外部匹配密钥时添加 title.keys；兼容的已知 DLC 为可选内容。"
  ],
  "dmWebGuideExtracted": [
    "Already-merged ExeFS/RomFS folders also work without keys, for base/update only, not expansion content. The format is detected automatically.",
    "也可使用已合并的 ExeFS/RomFS 文件夹，无需密钥，仅支持基础游戏及更新，不支持扩展内容。输入格式会自动识别。"
  ],
  "dmWebGuideFolderAccess": [
    "Use desktop Chrome or Edge, allow read/write access to the folder, and leave space for exported resources and temporary files.",
    "使用桌面版 Chrome 或 Edge，允许读写所选文件夹，并预留资源导出与临时文件所需空间。"
  ],
  "dmWebGuideExport": [
    "For a first install, leave “Skip resource export” off. Skip only when matching resources are already on the device; the browser cannot check them.",
    "首次安装请勿勾选“跳过资源导出”。仅在设备上已有完全匹配的资源时跳过；浏览器无法检查设备上的资源。"
  ],
  "dmWebGuideOutputs": [
    "You will download an IPA; resources are saved separately inside the selected input folder, not inside the IPA. After generation, follow the instructions with your actual output names to install the app and copy the completed resources.",
    "生成后可下载 IPA；资源另存到所选输入文件夹内，不在 IPA 中。完成后，请按显示的实际文件名安装应用，并复制已完整导出的资源。"
  ],
  "dmWebGuideLegacy": [
    "The older-GPU option also exports a separate supplemental folder. Copy it alongside the completed resources when the result says it is ready.",
    "旧 GPU 选项还会导出单独的补充文件夹。结果显示已完成时，将其与完整资源一同复制。"
  ],
  "dmWebGuideFirstLaunch": [
    "On first launch, “No game resources yet” is expected until the separate resources are copied. The install package does not contain the full game resources.",
    "第一次打开时，在尚未复制资源的情况下出现“暂无游戏资源”提示是正常的。安装包本身不含完整游戏资源。"
  ],
  "dmWebGuideDestination": [
    "Open Files → On My iPhone / iPad → the app’s folder (D2R Mobile, or your custom app name).",
    "打开“文件”→“我的 iPhone / iPad”→应用文件夹（D2R Mobile 或自定义应用名）。"
  ],
  "dmWebGuideCopyWhole": [
    "Copy the entire completed resource folder, preserving its name and inner data folder. Copy any completed older-device pack to the same app folder too. Do not use an incomplete export.",
    "请整体复制已完成的资源文件夹，保留名称及内部 data 文件夹结构。已完成的旧设备补充包也应复制到同一应用文件夹；不要使用未完成的导出。"
  ],
  "dmWebGuideRetry": [
    "Wait for copying to finish, then return to the app and tap Retry. The phone does not need NSP/XCI or key files.",
    "等待复制完成，然后返回应用点“重试”。手机上不需要 NSP/XCI 或密钥文件。"
  ],
  "dmWebOverallTitle": [
    "Overall generation",
    "生成总进度"
  ],
  "dmWebOverallEstimate": [
    "Stage-based estimate, not time remaining. Stages can take very different amounts of time.",
    "按已完成阶段估算，并非剩余时间；各阶段耗时可能差异很大。"
  ],
  "dmWebOverallCount": [
    "%s%% · %s/%s stages complete",
    "%s%% · 已完成 %s/%s 个阶段"
  ],
  "dmWebOverallUnmeasured": [
    "This stage has no measurable total; progress advances when it finishes.",
    "此阶段没有可量化的总量，完成后总进度才会前进。"
  ],
  "dmWebOverallPreparing": [
    "Preparing this generation job",
    "正在准备本次生成"
  ],
  "dmWebOverallInput": [
    "Checking and extracting game input",
    "正在检查并提取游戏输入"
  ],
  "dmWebOverallFinalize": [
    "Finalizing the IPA and app options",
    "正在完成 IPA 与应用选项"
  ],
  "dmWebOverallLegacy": [
    "Saving the ASTC supplemental pack",
    "正在保存 ASTC 补充包"
  ],
  "dmWebOverallSigning": [
    "Signing the requested IPA",
    "正在签名所需的 IPA"
  ],
  "dmWebOverallDownload": [
    "Preparing the IPA download link",
    "正在准备 IPA 下载链接"
  ],
  "dmWebOverallReady": [
    "Requested IPA ready to download. Installation and resource copying are separate manual steps.",
    "所需的 IPA 已可下载。安装与复制资源仍需手动完成。"
  ],
  "dmWebOverallFailed": [
    "Generation did not finish. See the status below.",
    "生成未完成。请查看下方状态。"
  ],
  "dmWebOverallCancelled": [
    "Generation cancelled; the requested output is not complete.",
    "生成已取消，所需的输出尚未完成。"
  ],
  "dmWebOverallSigningIncomplete": [
    "Signed output is not complete. The unsigned IPA is available; retry signing below.",
    "签名输出尚未完成。未签名 IPA 已保留，可在下方重试签名。"
  ],
  "dmWebOverallDownloadPending": [
    "The IPA is retained, but its download link is not ready. Retry download preparation below.",
    "IPA 已保留，但下载链接尚未就绪。请在下方重试准备下载。"
  ],
  "dmWebOverallSigningStage": [
    "Signing step %s/%s",
    "签名步骤 %s/%s"
  ],
  "dmWebGuideTargets": [
    "The default target is iOS 26+; iOS 18+ compatibility is also available. Standard output requires BC texture support. The older-GPU option is experimental, with device compatibility unverified.",
    "默认目标为 iOS 26+，也支持选择 iOS 18+ 兼容模式。标准输出需要 GPU 支持 BC 纹理；旧 GPU 选项仍为实验性功能，设备兼容性未经验证。"
  ],
  "dmWebGuideTrust": [
    "If requested, enable Settings → Privacy & Security → Developer Mode, restart and confirm. For Untrusted Developer, trust your signing account in Settings → General → VPN & Device Management.",
    "若提示启用开发者模式：设置 → 隐私与安全性 → 开发者模式，然后重新启动并确认。若提示开发者不受信任：设置 → 通用 → VPN 与设备管理，信任用于签名的账户。"
  ],
  "dmWebGuideRefresh": [
    "Renew expired signing credentials using the same team and bundle ID, and back up saves. Do not delete the app just to renew its signature: its saves and resources can be removed.",
    "签名凭据到期后，请沿用同一团队和 Bundle ID 更新签名，并备份存档。不要仅为更新签名而删除应用，否则其中的存档与资源可能一并删除。"
  ],
  "dmWebGuideSigned": [
    "For a signed IPA, the signature and profile must cover your device. On Mac, connect your device and use Apple Configurator → Add → Apps → Choose from my Mac → select the IPA. Signing alone does not install it.",
    "已签名 IPA 的签名与描述文件必须适用于该设备。Mac 可连接设备，使用 Apple Configurator → 添加 → App → 从我的 Mac 中选取 → 选择 IPA。签名本身不会安装应用。"
  ],
  "dmWebGuideTransfer": [
    "Mac: connect by USB, select your device in Finder → Files, and drag the whole resource folder onto the app name. Windows: choose the app in Apple Devices → device → Files; use the ZIP method if folders are not accepted. File sharing transfers resources, not signing/installing an unsigned IPA.",
    "Mac：用 USB 连接设备，在 Finder 中选择设备 → 文件，将完整资源文件夹拖到应用名上。Windows：在 Apple Devices → 设备 → 文件中选择应用；若不接受文件夹，可使用 ZIP 方式。文件共享用于传资源，不会为未签名 IPA 签名安装。"
  ],
  "dmWebGuideZip": [
    "If whole-folder transfer is unavailable, zip the complete folder, drag the ZIP into the app’s file-sharing area, then tap it in the app folder in Files to extract. Move the resource folder out of any extra ZIP wrapper so it sits directly in the app folder. Allow space for both ZIP and extracted files.",
    "若不能直接传整个文件夹，可将完整文件夹压成 ZIP，拖入上述应用文件共享区，再到手机的应用文件夹中点 ZIP 解压。将资源文件夹移出多余的 ZIP 外层目录，保持其直接位于应用文件夹内；预留 ZIP 与解压文件所需空间。"
  ],
  "dmWebGuideConfiguratorLink": [
    "Signed IPA installation",
    "已签名 IPA 安装说明"
  ],
  "dmWebGuideMacTransferLink": [
    "Mac file sharing",
    "Mac 传文件说明"
  ],
  "dmWebGuideWindowsTransferLink": [
    "Windows file sharing",
    "Windows 传文件说明"
  ],
  "dmWebGuideUnzipLink": [
    "Unzip in Files",
    "在“文件”中解压 ZIP"
  ],
  "dmWebBrowserOnly": [
    "Build your IPA entirely in your browser. No Xcode, Python, .NET installation, or command-line tools required.",
    "直接在浏览器中生成 IPA，无需安装 Xcode、Python 或 .NET，也不用操作命令行。"
  ],
  "dmWebGuideOptionalSigning": [
    "Prepare signing files, only if you want to sign here",
    "若要在此签名，再准备签名文件"
  ],
  "dmWebGuideSigningIntro": [
    "Signing is optional; unsigned IPA output is the default. If you already have your own P12/PFX and matching profile, go straight to the signing inputs.",
    "签名为可选功能，默认生成未签名 IPA。若已有自己的 P12/PFX 及匹配的描述文件，可直接选择签名文件。"
  ],
  "dmWebGuideIdentity": [
    "Use a signing certificate with its private key, not a .cer file alone. On Mac, select both in Keychain Access → File → Export Items and save as a password-protected P12.",
    "签名需要证书及其私钥，只有 .cer 文件不够。Mac 可在“钥匙串访问”中选中两者，通过“文件”→“导出项目”保存为设有密码的 P12。"
  ],
  "dmWebGuideNewCertificate": [
    "For a new identity, use your Apple Developer Program team with Account Holder/Admin access: create a private key and CSR locally, submit only the CSR for an Apple Distribution certificate, then export the certificate with that same key as P12. Without a Mac, this preparation can use OpenSSL.",
    "若需新建身份，请使用 Apple Developer Program 团队的持有人或管理员权限：在本地创建私钥与 CSR，仅向 Apple 提交 CSR 以申请 Apple Distribution 证书，再将证书与原私钥导出为 P12。没有 Mac 时，可使用 OpenSSL 完成这项准备。"
  ],
  "dmWebGuideProfile": [
    "Register your device’s UDID and an App ID under that team. In Profiles → + → Ad Hoc, select that App ID, the same certificate and your device, then download .mobileprovision. Match the app’s Bundle ID in customization to the profile’s App ID.",
    "在该团队下注册设备 UDID 与 App ID。在 Profiles → + → Ad Hoc 中选择该 App ID、同一证书及设备，下载 .mobileprovision。自定义设置中的应用 Bundle ID 必须与描述文件的 App ID 匹配。"
  ],
  "dmWebGuideSigningInputs": [
    "Enable local signing, choose the P12/PFX and profile, and enter the P12 export password, not your Apple Account password. Leave it empty only for a passwordless identity. The browser cannot verify device eligibility.",
    "启用本地签名，选择 P12/PFX 与描述文件，并输入 P12 导出密码，而非 Apple 账户密码。仅无密码的身份文件可留空。浏览器无法验证设备是否获准安装。"
  ],
  "dmWebGuidePrivateKey": [
    "Keep your private key and export password local. Do not upload them to online certificate converters or signing services.",
    "请将私钥与导出密码保留在本地，不要上传到在线证书转换或签名服务。"
  ],
  "dmWebGuideExpect": [
    "Generate, then install and copy",
    "生成后再安装并复制资源"
  ],
  "dmWebGuideCertificateLink": [
    "Apple: create a CSR",
    "Apple：创建 CSR"
  ],
  "dmWebGuideP12Link": [
    "Mac: export a P12",
    "Mac：导出 P12"
  ],
  "dmWebGuideOpenSslLink": [
    "OpenSSL: P12 export",
    "OpenSSL：导出 P12"
  ],
  "dmWebGuideProfileLink": [
    "Apple: Ad Hoc profile",
    "Apple：Ad Hoc 描述文件"
  ],
  "dmWebFaqTitle": [
    "FAQ",
    "常见问题"
  ],
  "dmWebFaqSigningQuestion": [
    "Do I have to sign the IPA here?",
    "必须在这里为 IPA 签名吗？"
  ],
  "dmWebFaqSigningAnswer": [
    "No. Unsigned output is the default. Optional local signing uses your own P12/PFX, export password and provisioning profile through the bundled zsign signer. A signed IPA still needs installation; signing does not guarantee your device will accept it.",
    "不必，默认生成未签名 IPA。可选本地签名使用自带的 zsign，配合你自己的 P12/PFX、导出密码与描述文件。已签名 IPA 仍需安装；签名成功并不保证设备一定接受。"
  ],
  "dmWebFaqInputsQuestion": [
    "Which game files and keys are supported?",
    "支持哪些游戏文件与密钥？"
  ],
  "dmWebFaqInputsAnswer": [
    "Use supported base/update NSP/XCI with prod.keys; title.keys is needed only when a matching external title key is required. Compatible known DLC is optional. Already-merged ExeFS/RomFS needs no keys but supports base/update only.",
    "可使用支持的基础游戏及更新 NSP/XCI，配合 prod.keys；仅需外部匹配标题密钥时才提供 title.keys。兼容的已知 DLC 为可选内容。已合并的 ExeFS/RomFS 无需密钥，但仅支持基础游戏及更新。"
  ],
  "dmWebFaqIosQuestion": [
    "Is iOS 18 supported too?",
    "也支持 iOS 18 吗？"
  ],
  "dmWebFaqAssetsQuestion": [
    "Where are the resources, and when do I copy them?",
    "资源在哪里，什么时候复制？"
  ],
  "dmWebFaqAssetsAnswer": [
    "Resources are separate from the IPA, inside your selected input folder. Install and open the app first, then copy the completed folders into its Files app folder. The result panel shows the actual names, including any completed older-device pack. Your phone does not need the original NSP/XCI or keys.",
    "资源与 IPA 分开，保存在所选输入文件夹内。先安装并打开应用，再将已完成的文件夹复制到“文件”中的应用目录。结果区域会显示实际名称，包括已完成的旧设备补充包。手机不需要原始 NSP/XCI 或密钥。"
  ],
  "dmWebFaqPrivacyQuestion": [
    "Are my files or signing credentials uploaded?",
    "文件或签名凭据会上传吗？"
  ],
  "dmWebFaqPrivacyAnswer": [
    "No. Public runtime files are downloaded, but game files, keys and signing inputs are processed locally in your browser. The P12 and password are not uploaded or included in the IPA. The provisioning profile is embedded in the signed IPA, so be careful when sharing it.",
    "不会。网页会下载公开运行组件，但游戏文件、密钥和签名输入均在浏览器本地处理。P12 与密码不会上传，也不会写入 IPA；描述文件会嵌入已签名 IPA，因此分享 IPA 时请留意。"
  ],
  "dmWebResultTitle": [
    "Next: install and copy files",
    "下一步：安装并复制文件"
  ],
  "dmWebResultInstall": [
    "Install %s on your device. Signing and installation are separate.",
    "将 %s 安装到设备。签名与安装是分开的操作。"
  ],
  "dmWebResultUnsigned": [
    "This IPA is unsigned. Sign it with your own certificate and provisioning profile before installing; you can enable local signing here.",
    "此 IPA 尚未签名。安装前请使用自己的证书与描述文件签名；也可在此启用本地签名。"
  ],
  "dmWebResultLaunch": [
    "Open %s once, then close it. Its folder should appear in Files → On My iPhone / iPad.",
    "打开 %s 一次，然后关闭。它的文件夹应出现在“文件”→“我的 iPhone / iPad”中。"
  ],
  "dmWebResultCopy": [
    "Copy the whole %s folder into %s’s app folder. Keep its name and contents unchanged.",
    "将整个 %s 文件夹复制到 %s 的应用文件夹中，保留原有名称和内容。"
  ],
  "dmWebResultSkipped": [
    "No new resource folder was exported. Keep the complete matching resources already on your device.",
    "此次未导出新的资源文件夹，请保留设备上已有的完整匹配资源。"
  ],
  "dmWebResultIncomplete": [
    "The resource export is incomplete. Do not copy or use its partial folder; generate a complete export first.",
    "资源导出未完成，请勿复制或使用不完整的文件夹；请先完成资源导出。"
  ],
  "dmWebResultLegacyIncomplete": [
    "The older-device pack did not finish. Do not copy its partial folder.",
    "旧设备补充包未完成，请勿复制其中不完整的文件夹。"
  ],
  "dmWebGenerateSigningFilesNeeded": [
    "To generate with local signing, choose a .p12/.pfx certificate and a .mobileprovision profile in Signing, or turn local signing off to generate an unsigned IPA.",
    "若要在本地签名后生成，请在“签名”中选择 .p12/.pfx 证书与 .mobileprovision 描述文件；也可关闭本地签名，生成未签名 IPA。"
  ],
  "dmWebGenerateSigningIdentityNeeded": [
    "Choose a .p12/.pfx certificate in Signing, or turn local signing off to generate an unsigned IPA.",
    "请在“签名”中选择 .p12/.pfx 证书；也可关闭本地签名，生成未签名 IPA。"
  ],
  "dmWebGenerateSigningProfileNeeded": [
    "Choose a .mobileprovision profile in Signing, or turn local signing off to generate an unsigned IPA.",
    "请在“签名”中选择 .mobileprovision 描述文件；也可关闭本地签名，生成未签名 IPA。"
  ],
  "dmWebGenerateSigningUnavailable": [
    "Local signing is not available. Turn it off in Signing to generate an unsigned IPA.",
    "本地签名不可用。请在“签名”中将其关闭，以生成未签名 IPA。"
  ],
  "dmWebGenerateChooseFolder": [
    "Choose a valid game folder first.",
    "请先选择有效的游戏文件夹。"
  ],
  "dmWebGenerateReady": [
    "Ready to generate.",
    "可以生成。"
  ],
  "dmWebIgnoreCache": [
    "Ignore saved build cache",
    "忽略已保存的生成缓存"
  ],
  "dmWebIgnoreCacheHelp": [
    "Rebuild generated files instead of reusing saved results. Existing files are not deleted; new accepted results may update the cache. Verified public runtime downloads are unaffected.",
    "重新生成文件，不复用已保存的结果。不会删除现有文件；新通过校验的结果可能更新缓存。已验证的公开运行组件下载缓存不受影响。"
  ],
  "dmWebIgnoreCacheActive": [
    "This run will not reuse saved build results; accepted new results may still update the cache.",
    "本次生成不复用已保存的生成缓存；通过校验的新结果仍可更新缓存。"
  ],
  "dmWebFaqCacheQuestion": [
    "How do I troubleshoot cached build results?",
    "重复生成遇到问题，如何处理缓存？"
  ],
  "dmWebFaqCacheAnswer": [
    "First try Ignore saved build cache in Advanced. If cleanup is still needed, stop generation and close other packager tabs, then delete only the D2R-cache folder inside your selected game folder and retry. Do not delete data, older D2R-resources-* exports, Legacy Device Pack folders, original game files, keys, or saves. Verified public runtime downloads in the browser are separate.",
    "先尝试在“高级选项”中勾选“忽略已保存的生成缓存”。如仍需清理，请先停止生成并关闭其他打包页面，然后仅删除所选游戏文件夹内的 D2R-cache 文件夹，再重试。不要删除 data、旧的 D2R-resources-* 导出目录、Legacy Device Pack 文件夹、原始游戏文件、密钥或存档。此操作与浏览器中已验证的公开运行组件下载缓存无关。"
  ],
  "dmWebCacheUnavailable": [
    "Saved build cache is unavailable; continuing without it.",
    "已保存的生成缓存不可用，将继续生成而不使用缓存。"
  ],
  "dmWebGenerationExpectation": [
    "Keep this tab in the foreground while generating. The first build may take 30 minutes or longer, depending on your computer.",
    "请在生成期间保持此标签页处于前台。首次生成可能需要 30 分钟或更长时间，具体取决于你的电脑。"
  ],
  "dmWebUnsignedFallbackDownload": [
    "Download unsigned IPA instead",
    "改为下载未签名 IPA"
  ],
  "dmWebUnsignedFallbackHelp": [
    "Local signing did not finish. Retry signing above, or download this unsigned IPA to sign with another tool. It is not ready to install.",
    "本地签名尚未完成。请在上方重试签名，或下载此未签名 IPA 后使用其他工具签名。它尚不能直接安装。"
  ],
  "dmWebResourceVerifyProgress": [
    "Checking existing resources: %s/%s files; %s/%s MB",
    "正在检查已有资源：%s/%s 个文件；%s/%s MB"
  ],
  "dmWebExistingDataUnverified": [
    " The existing data folder could not be verified against this input. Its files were not changed. Move it aside before exporting again, or select Skip resource export (already on device) if you already have a complete matching copy.",
    " 现有 data 文件夹无法通过与本次输入的比对，其中的文件未被更改。请先将它移到其他位置再导出；如果已有完整匹配的资源，也可勾选“跳过资源导出（设备上已有）”。"
  ],
  "dmWebResourceExportBusy": [
    " Another tab is exporting resources. This attempt did not change resource files; wait for that export to finish and retry.",
    " 另一个标签页正在导出资源。本次没有改动资源文件，请等待该导出结束后重试。"
  ],
  "dmWebResourceExportLockUnavailable": [
    " This browser cannot safely lock resource export, so resource files were not changed. Use a desktop Chromium browser with Web Locks support.",
    " 当前浏览器无法安全锁定资源导出，因此没有改动资源文件。请使用支持 Web Locks 的桌面 Chromium 浏览器。"
  ],
  "dmWebLanguageLabel": [
    "Language",
    "语言"
  ],
  "dmWebLanguageEnglish": [
    "English",
    "英语"
  ],
  "dmWebLanguageChinese": [
    "Simplified Chinese",
    "简体中文"
  ],
  "dmWebFolderAccessUnsupported": [
    "This browser cannot read and write the selected game folder, so IPA generation is unavailable here. Open this page in desktop Chrome or Edge on Windows or macOS. You can still change the language and read the guide and FAQ.",
    "当前浏览器无法读写所选游戏文件夹，因此无法在这里生成 IPA。请在 Windows 或 macOS 的桌面版 Chrome 或 Edge 中打开此页面。你仍可切换语言、阅读指南和常见问题。"
  ]
});
