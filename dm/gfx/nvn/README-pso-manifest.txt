pso_vk_manifest.bin —— Vulkan 管线预热种子清单（方案 A）

用途
    随包发布，与 shader 缓存同目录（<shader_root>/pso_vk_manifest.bin）。
    启动后首次出图时读取，在渲染线程上分帧重建清单里的管线，让「第一次见怪 /
    进新场景」不再现场编译（实测现编中位 5ms、最大 27.5ms）。

    运行目录 (<diagnostics>/pso-vk-warm.bin) 里的用户清单会追加合并，
    所以玩家自己玩出来的新管线也会记进下一次启动。

格式（小端，二进制）
    uint32 magic = 0x57504D44 ("DMPW")
    uint32 version = 1
    uint32 count
    count × WarmRec：
        PsoKey key;        // 全部创建参数，POD：顶点输入/光栅/深度模板/混合/
                           // 动态状态之外的固定状态 + PassKey（渲染通道自包含）
        uint64 hash;       // 由 key 算出的哈希（重建后按当前索引重算）
        char   vs[17];     // 顶点着色器内容键（<key>.vkmeta / .spv[.gz] 的文件名主干）
        char   ps[17];     // 片元着色器内容键（has_ps=0 时为空）
        uint8  vs_stage, ps_stage, has_ps, pad;

可移植性
    重建是在本机按着色器内容键重新编译，不依赖生成清单时的 GPU/驱动，
    所以同一个种子可以发给不同设备（与本仓库 Metal 侧 pso_manifest.tsv 同理）。

不要穷举
    着色器两两组合是几十万量级，实际只用到几百对。只记录真实建成过的管线。

关掉
    DM_PSO_WARM=off
