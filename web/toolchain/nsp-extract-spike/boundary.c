/* 有界拉取 API，依据固定版本 hactool 的 NCA、PFS0 与 IVFC 接口。
 * 上游 CLI 主入口、自动密钥搜索、提取路径、标准输出或错误、网络调用及持久密钥文件均不可达。
 * 返回码：0 成功；-1 状态或输入；-2 格式或范围；-3 加密或完整性；
 * -4 不支持；-5 I/O；-6 所需标题密钥不可用。负数错误绝不包含用户输入。
 * Bounded pull API based on pinned hactool NCA, PFS0 and IVFC interfaces.
 * No upstream CLI main, auto key search, extraction paths,
 * stdout/stderr, network calls or persistent key files are reachable here.
 * Return codes: 0 OK; -1 state/input; -2 format/bounds; -3 crypto/integrity;
 * -4 unsupported; -5 I/O; -6 required title key is unavailable. Negative
 * failures never include user input.
 */
#include "extkeys.h"
#include "nca.h"
#include "pki.h"
#include "rsa.h"
#include "sha.h"
#include <limits.h>
#include <stdint.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
/* 仅报告固定数字进度；由 Worker 提供受信回调。
 * Fixed numeric progress only. The worker supplies the trusted callback. */
// clang-format off
EM_JS(void, nsp_ivfc_report, (int phase, double verified, double total), {
    Module["onVerificationProgress"]?.(phase, verified, total);
});
// clang-format on
#else
static void nsp_ivfc_report(int phase, double verified, double total) {
    (void)phase;
    (void)verified;
    (void)total;
}
#endif
#define META_LIMIT (16u * 1024u * 1024u)
#define CHUNK_LIMIT (64u * 1024u)
static hactool_ctx_t tool;
static nca_ctx_t nca;
static nca_section_ctx_t *section;
static FILE *raw;
static uint64_t extent, pfs_offset, romfs_offset, romfs_size;
static pfs0_header_t *directory;
static size_t directory_size;
static int keys_ready;
extern void nsp_clear_parser(void);
void nsp_zero(void *ptr, size_t size) {
    volatile unsigned char *p = ptr;
    while (size--)
        *p++ = 0;
}
void *nsp_resize_keys(void *ptr, size_t old_size, size_t new_size) {
    void *next = calloc(1, new_size);
    if (!next)
        return NULL;
    if (ptr) {
        memcpy(next, ptr, old_size);
        nsp_zero(ptr, old_size);
        free(ptr);
    }
    return next;
}
static int within(uint64_t at, uint64_t len, uint64_t size) {
    return at <= size && len <= size - at;
}
static uint64_t pair(uint32_t lo, uint32_t hi) { return ((uint64_t)hi << 32) | lo; }
#include "bktr-reader.h"
static int read_merged(uint64_t offset, void *out, size_t size);
void nsp_close(void) {
    close_patch();
    if (directory) {
        free(directory);
        directory = NULL;
    }
    directory_size = 0;
    if (section && section->aes)
        free_aes_ctx(section->aes);
    if (nca.file)
        fclose(nca.file);
    if (raw)
        fclose(raw);
    nsp_zero(&nca, sizeof nca);
    section = NULL;
    raw = NULL;
    extent = 0;
    pfs_offset = 0;
    romfs_offset = 0;
    romfs_size = 0;
}
void nsp_reset(void) {
    nsp_close();
    if (tool.settings.known_titlekeys.titlekeys) {
        nsp_zero(tool.settings.known_titlekeys.titlekeys,
                 tool.settings.known_titlekeys.count * sizeof(titlekey_entry_t));
        free(tool.settings.known_titlekeys.titlekeys);
    }
    nsp_zero(&tool, sizeof tool);
    nsp_clear_parser();
    keys_ready = 0;
}
int nsp_load_keys(void) {
    nsp_reset();
    FILE *prod = fopen("/secret/prod.keys", "rb"), *title = fopen("/secret/title.keys", "rb");
    if (!prod || !title) {
        if (prod)
            fclose(prod);
        if (title)
            fclose(title);
        return -5;
    }
    /* 避免在 libc 读取缓冲区中保留额外的密钥文件副本。
     * Avoid retaining additional key-file copies in libc read buffers. */
    setvbuf(prod, NULL, _IONBF, 0);
    setvbuf(title, NULL, _IONBF, 0);
    tool.settings.skip_key_warnings = 1;
    tool.settings.suppress_keydata_output = 1;
    pki_initialize_keyset(&tool.settings.keyset, KEYSET_RETAIL);
    extkeys_initialize_settings(&tool.settings, prod);
    fclose(prod);
    extkeys_parse_titlekeys(&tool.settings, title);
    fclose(title);
    nsp_clear_parser();
    pki_derive_keys(&tool.settings.keyset);
    keys_ready = 1;
    return 0;
}
static int read_at(uint64_t offset, void *out, size_t size) {
    if (!within(offset, size, extent))
        return -2;
    if (merged_active)
        return read_merged(offset, out, size);
    if (section) {
        nca_section_fseek(section, offset);
        return nca_section_fread(section, out, size) == size ? 0 : -5;
    }
    if (!raw || fseeko(raw, (off_t)offset, SEEK_SET))
        return -5;
    return fread(out, 1, size, raw) == size ? 0 : -5;
}
/* 同一个有界读取器用于已认证散列层级及公开输入或条目摘要。
 * 不分配完整的可执行文件或分区。
 * The same bounded reader serves authenticated hash hierarchies and public
 * input/entry digests. No complete executable or section is allocated. */
static int hash_range(uint64_t offset, uint64_t size, unsigned char digest[32]) {
    if (!within(offset, size, extent))
        return -2;
    unsigned char *chunk = malloc(CHUNK_LIMIT);
    if (!chunk)
        return -1;
    sha_ctx_t *hash = new_sha_ctx(HASH_TYPE_SHA256, 0);
    int rc = 0;
    for (uint64_t done = 0; done < size;) {
        size_t count = size - done > CHUNK_LIMIT ? CHUNK_LIMIT : (size_t)(size - done);
        rc = read_at(offset + done, chunk, count);
        if (rc)
            break;
        sha_update(hash, chunk, count);
        done += count;
    }
    if (!rc)
        sha_get_hash(hash, digest);
    free_sha_ctx(hash);
    nsp_zero(chunk, CHUNK_LIMIT);
    free(chunk);
    return rc;
}
static int compare_names(const void *a, const void *b) {
    const pfs0_file_entry_t *left = *(const pfs0_file_entry_t *const *)a;
    const pfs0_file_entry_t *right = *(const pfs0_file_entry_t *const *)b;
    const char *strings = pfs0_get_string_table(directory);
    return strcmp(strings + left->string_table_offset, strings + right->string_table_offset);
}
static int compare_offsets(const void *a, const void *b) {
    const pfs0_file_entry_t *left = *(const pfs0_file_entry_t *const *)a;
    const pfs0_file_entry_t *right = *(const pfs0_file_entry_t *const *)b;
    if (left->offset != right->offset)
        return left->offset < right->offset ? -1 : 1;
    return left->size < right->size ? -1 : left->size > right->size ? 1 : 0;
}
static int load_directory(void) {
    pfs0_header_t head;
    if (read_at(pfs_offset, &head, sizeof head))
        return -5;
    if (head.magic != MAGIC_PFS0 || head.num_files > 100000)
        return -2;
    uint64_t bytes =
        sizeof head + (uint64_t)head.num_files * sizeof(pfs0_file_entry_t) + head.string_table_size;
    if (bytes > META_LIMIT || !within(pfs_offset, bytes, extent))
        return -2;
    directory = malloc((size_t)bytes);
    if (!directory)
        return -1;
    directory_size = (size_t)bytes;
    if (read_at(pfs_offset, directory, directory_size))
        return -5;
    if (memcmp(directory, &head, sizeof head))
        return -2;
    char *strings = pfs0_get_string_table(directory);
    for (uint32_t i = 0; i < head.num_files; i++) {
        pfs0_file_entry_t *e = pfs0_get_file_entry(directory, i);
        if (e->string_table_offset >= head.string_table_size ||
            (e->string_table_offset && strings[e->string_table_offset - 1] != 0))
            return -2;
        const char *name = strings + e->string_table_offset;
        /* 与生产 JS 输入策略一致：名称最多 255 字节，偏移必须位于字符串起点。
         * Match production JS input policy: at most 255 name bytes, at a string start.
         * 限制每项扫描，避免对恶意偏移反复扫描整个元数据表。
         * Bound each scan instead of repeatedly scanning the full table for hostile offsets. */
        size_t name_limit = head.string_table_size - e->string_table_offset;
        if (name_limit > 256)
            name_limit = 256;
        const char *end = memchr(name, 0, name_limit);
        if (!end || end == name || !strcmp(name, ".") || !strcmp(name, ".."))
            return -2;
        for (const char *p = name; p < end; p++)
            if ((unsigned char)*p < 32 || (unsigned char)*p == 127 || *p == '/' || *p == '\\' ||
                *p == ':')
                return -2;
        if (!within(pfs_offset + bytes, e->offset, extent) ||
            !within(pfs_offset + bytes + e->offset, e->size, extent))
            return -2;
    }
    if (head.num_files > 1) {
        pfs0_file_entry_t **sorted = malloc(head.num_files * sizeof(*sorted));
        if (!sorted)
            return -1;
        for (uint32_t i = 0; i < head.num_files; i++)
            sorted[i] = pfs0_get_file_entry(directory, i);
        qsort(sorted, head.num_files, sizeof(*sorted), compare_names);
        int rc = 0;
        for (uint32_t i = 1; i < head.num_files; i++)
            if (!compare_names(&sorted[i - 1], &sorted[i])) {
                rc = -2;
                break;
            }
        qsort(sorted, head.num_files, sizeof(*sorted), compare_offsets);
        uint64_t prior_end = 0;
        for (uint32_t i = 0; !rc && i < head.num_files; i++) {
            if (sorted[i]->size && sorted[i]->offset < prior_end)
                rc = -2;
            if (sorted[i]->size)
                prior_end = sorted[i]->offset + sorted[i]->size;
        }
        free(sorted);
        if (rc)
            return rc;
    }
    return 0;
}
int nsp_open_pfs0(const char *path) {
    nsp_close();
    raw = fopen(path, "rb");
    if (!raw)
        return -5;
    if (fseeko(raw, 0, SEEK_END))
        return -5;
    off_t end = ftello(raw);
    if (end < 0)
        return -5;
    extent = (uint64_t)end;
    int result = load_directory();
    if (result)
        nsp_close();
    return result;
}
static int open_header(const char *path) {
    nsp_close();
    if (!keys_ready)
        return -1;
    nca.tool_ctx = &tool;
    nca.file = fopen(path, "rb");
    if (!nca.file)
        return -5;
    if (fseeko(nca.file, 0, SEEK_END))
        return -5;
    off_t end = ftello(nca.file);
    if (end < 0xC00)
        return -2;
    extent = (uint64_t)end;
    if (!nca_decrypt_header(&nca))
        return -3;
    if (nca.header.magic != MAGIC_NCA3 && nca.header.magic != MAGIC_NCA2)
        return -4;
    if (nca.header.nca_size != extent || nca.header.kaek_ind >= 3)
        return -2;
    unsigned gen = nca.header.crypto_type > nca.header.crypto_type2 ? nca.header.crypto_type
                                                                    : nca.header.crypto_type2;
    if (gen)
        gen--;
    if (gen >= 32)
        return -4;
    nca.crypto_type = (unsigned char)gen;
    for (unsigned i = 0; i < 16; i++)
        if (nca.header.rights_id[i])
            nca.has_rights_id = 1;
    return 0;
}
/* 仅公开字段，共 24 字节：u64 标题 ID、u64 大小、u32 内容类型、
 * u32 固定签名有效标记。头部检查并不等于内容认证。
 * Public fields only, 24 bytes: u64 title ID, u64 size, u32 content type,
 * u32 fixed-signature-valid. Header inspection is NOT content authentication.
 */
int nsp_inspect_nca(const char *path, unsigned char *out) {
    int rc = open_header(path);
    if (rc)
        return rc;
    uint32_t type = nca.header.content_type, valid = 0;
    if (nca.header.fixed_key_generation < 2)
        valid = rsa2048_pss_verify(
                    &nca.header.magic, 0x200, nca.header.fixed_key_sig,
                    tool.settings.keyset.nca_hdr_fixed_key_moduli[nca.header.fixed_key_generation])
                    ? 1
                    : 0;
    memcpy(out, &nca.header.title_id, 8);
    memcpy(out + 8, &nca.header.nca_size, 8);
    memcpy(out + 16, &type, 4);
    memcpy(out + 20, &valid, 4);
    return 0;
}
/* 共用的签名分区打开接口：0 Meta、1 Program ExeFS、2 基础 RomFS、
 * 3 旧版 BKTR RomFS。这里只读取选定分区的内容。
 * Shared signed section opener: 0 Meta, 1 Program ExeFS, 2 base RomFS,
 * 3 legacy BKTR RomFS. Only selected section content is read here. */
static int open_authenticated_section(const char *path, int mode, int warlock_meta) {
    int program = mode != 0;
    unsigned char public_info[24];
    int rc = nsp_inspect_nca(path, public_info);
    if (rc)
        return rc;
    uint32_t valid;
    memcpy(&valid, public_info + 20, 4);
    /* 已知仅用于解锁的 Warlock Meta 可能经过重新打包而不带零售版签名。
     * 调用方会计算整个 NCA 的摘要并检查其 CNMT 内容。
     * 绝不可将此例外扩展到可执行内容或 RomFS 内容。
     * The known unlock-only Warlock Meta may be repacked without a retail
     * signature. Its caller measures the whole NCA and checks its CNMT content.
     * Never extend this exception to executable or RomFS content. */
    int unsigned_warlock_meta = warlock_meta && mode == 0;
    unsigned_warlock_meta &= nca.header.title_id == 0x0100726014353001ULL;
    unsigned_warlock_meta &= nca.header.content_type == NCACONTENTTYPE_META;
    if (!valid && !unsigned_warlock_meta)
        return -3;
    if (nca.header.content_type != (program ? NCACONTENTTYPE_PROGRAM : NCACONTENTTYPE_META))
        return -4;
    if (!nca.has_rights_id)
        nca_decrypt_key_area(&nca);
    unsigned found = 4, present = 0;
    for (unsigned i = 0; i < 4; i++) {
        if (!nca.header.section_entries[i].media_start_offset)
            continue;
        present++;
        nca_fs_header_t *candidate = &nca.header.fs_headers[i];
        unsigned char digest[32];
        sha256_hash_buffer(digest, candidate, sizeof(*candidate));
        if (memcmp(digest, nca.header.section_hashes[i], 32))
            return -3;
        uint64_t start = (uint64_t)nca.header.section_entries[i].media_start_offset * 512;
        uint64_t end = (uint64_t)nca.header.section_entries[i].media_end_offset * 512;
        if (start < 0xC00 || end <= start || end > extent)
            return -2;
        for (unsigned j = 0; j < i; j++) {
            if (!nca.header.section_entries[j].media_start_offset)
                continue;
            uint64_t other_start = (uint64_t)nca.header.section_entries[j].media_start_offset * 512;
            uint64_t other_end = (uint64_t)nca.header.section_entries[j].media_end_offset * 512;
            if (start < other_end && other_start < end)
                return -2;
        }
        if (candidate->crypt_type != CRYPT_NONE && candidate->crypt_type != CRYPT_CTR &&
            candidate->crypt_type != CRYPT_XTS &&
            !((mode == 3 || mode == 1) && candidate->crypt_type == CRYPT_BKTR))
            return -4;
        /* 这些旧版加密路径不实现稀疏或压缩分区，
         * 也不实现较新的元数据散列或跳层散列扩展。
         * These legacy crypto paths do not implement sparse/compressed
         * sections or the newer metadata-hash/skip-layer-hash extensions. */
        for (size_t j = 0; j < sizeof candidate->_0x5; j++)
            if (candidate->_0x5[j])
                return -4;
        for (size_t j = 0; j < sizeof candidate->_0x148; j++)
            if (candidate->_0x148[j])
                return -4;
        int is_pfs0 =
            candidate->partition_type == PARTITION_PFS0 && candidate->fs_type == FS_TYPE_PFS0;
        int is_romfs =
            candidate->partition_type == PARTITION_ROMFS && candidate->fs_type == FS_TYPE_ROMFS;
        if (!is_pfs0 && (!program || !is_romfs))
            return -4;
        if ((mode >= 2 && is_romfs) || (mode < 2 && is_pfs0)) {
            if (found != 4)
                return -4;
            found = i;
        }
    }
    if (!program && present != 1)
        return -4;
    if (found == 4)
        return -2;
    nca_fs_header_t *h = &nca.header.fs_headers[found];
    if (mode == 3 ? h->crypt_type != CRYPT_BKTR
                  : (h->crypt_type != CRYPT_CTR && h->crypt_type != CRYPT_NONE))
        return -4;
    unsigned char digest[32];
    sha256_hash_buffer(digest, h, sizeof *h);
    if (memcmp(digest, nca.header.section_hashes[found], 32))
        return -3;
    uint64_t start = (uint64_t)nca.header.section_entries[found].media_start_offset * 512;
    uint64_t end = (uint64_t)nca.header.section_entries[found].media_end_offset * 512;
    if (start < 0xC00 || end < start || end > extent)
        return -2;
    section = &nca.section_contexts[found];
    section->file = nca.file;
    section->tool_ctx = &tool;
    section->offset = start;
    section->size = end - start;
    section->header = h;
    section->crypt_type = mode == 3 ? CRYPT_CTR : h->crypt_type;
    section->is_decrypted = nca.is_decrypted || h->crypt_type == CRYPT_NONE;
    for (unsigned j = 0; j < 8; j++)
        section->ctr[j] = h->section_ctr[7 - j];
    if (!section->is_decrypted) {
        unsigned char key[16];
        if (nca.has_rights_id) {
            titlekey_entry_t *entry = settings_get_titlekey(&tool.settings, nca.header.rights_id);
            if (!entry)
                return -6;
            aes_ctx_t *aes =
                new_aes_ctx(tool.settings.keyset.titlekeks[nca.crypto_type], 16, AES_MODE_ECB);
            aes_decrypt(aes, key, entry->titlekey, 16);
            free_aes_ctx(aes);
        } else
            memcpy(key, nca.decrypted_keys[2], 16);
        section->aes = new_aes_ctx(key, 16, AES_MODE_CTR);
        nsp_zero(key, sizeof key);
    }
    extent = section->size;
    return 0;
}
static int open_authenticated_pfs0(const char *path, int program, int warlock_meta) {
    int rc = open_authenticated_section(path, program ? 1 : 0, warlock_meta);
    if (rc)
        return rc;
    nca_fs_header_t *h = section->header;
    unsigned char digest[32];
    pfs0_superblock_t *sb = &h->pfs0_superblock;
    if ((!program && sb->pfs0_size > META_LIMIT) || sb->hash_table_size > META_LIMIT ||
        !sb->block_size || sb->block_size > META_LIMIT ||
        !within(sb->hash_table_offset, sb->hash_table_size, extent) ||
        !within(sb->pfs0_offset, sb->pfs0_size, extent))
        return -2;
    uint64_t blocks = sb->pfs0_size / sb->block_size + (sb->pfs0_size % sb->block_size != 0);
    if (!blocks || blocks > sb->hash_table_size / 32)
        return -2;
    /* 拒绝与其认证数据重叠的散列表。
     * Reject a hash table overlapping the data it authenticates. */
    if (sb->hash_table_offset < sb->pfs0_offset + sb->pfs0_size &&
        sb->pfs0_offset < sb->hash_table_offset + sb->hash_table_size)
        return -2;
    unsigned char *hashes = malloc((size_t)sb->hash_table_size);
    if (!hashes)
        return -1;
    rc = read_at(sb->hash_table_offset, hashes, (size_t)sb->hash_table_size);
    if (!rc) {
        sha256_hash_buffer(digest, hashes, (size_t)sb->hash_table_size);
        if (memcmp(digest, sb->master_hash, 32))
            rc = -3;
    }
    for (uint64_t i = 0; !rc && i < blocks; i++) {
        uint64_t bytes = sb->block_size;
        if (bytes > sb->pfs0_size - i * sb->block_size)
            bytes = sb->pfs0_size - i * sb->block_size;
        rc = hash_range(sb->pfs0_offset + i * sb->block_size, bytes, digest);
        if (!rc && memcmp(digest, hashes + i * 32, 32))
            rc = -3;
    }
    free(hashes);
    if (rc)
        return rc;
    /* 将目录读取与成员限制在已验证的 PFS0 区域内。
     * Restrict directory reads and members to the verified PFS0 region. */
    pfs_offset = sb->pfs0_offset;
    extent = pfs_offset + sb->pfs0_size;
    rc = load_directory();
    if (!rc && program) {
        int main_found = 0, npdm_found = 0;
        for (uint32_t i = 0; i < directory->num_files; i++) {
            const char *name = pfs0_get_file_name(directory, i);
            uint32_t expected = !strcmp(name, "main")        ? 0x304F534E
                                : !strcmp(name, "main.npdm") ? 0x4154454D
                                                             : 0;
            if (!expected)
                continue;
            pfs0_file_entry_t *entry = pfs0_get_file_entry(directory, i);
            uint32_t magic = 0;
            if (entry->size < sizeof(magic) ||
                read_at(pfs_offset + directory_size + entry->offset, &magic, sizeof(magic)) ||
                magic != expected) {
                rc = -2;
                break;
            }
            if (expected == 0x304F534E)
                main_found = 1;
            else
                npdm_found = 1;
        }
        if (!main_found || !npdm_found)
            rc = -2;
    }
    if (rc)
        nsp_close();
    return rc;
}
/* IVFC 读取器约定：相对于分区的精确读取。只有在 NCA 与分区认证之后
 * 从 nsp_open_romfs 调用时，根才可信。
 * IVFC reader contract: section-relative exact reads. The root is trusted
 * only when called from nsp_open_romfs AFTER NCA/section authentication. */
typedef int (*ivfc_reader_t)(void *, uint64_t, void *, size_t);
static int ivfc_validate(const ivfc_hdr_t *ivfc, uint64_t size, uint64_t *out_total) {
    if (ivfc->magic != MAGIC_IVFC || ivfc->id != 0x20000 ||
        ivfc->num_levels != IVFC_MAX_LEVEL + 1 || ivfc->master_hash_size != 32)
        return -4;
    uint64_t total = 0;
    for (unsigned i = 0; i < IVFC_MAX_LEVEL; i++) {
        const ivfc_level_hdr_t *level = &ivfc->level_headers[i];
        if (!level->hash_data_size || level->block_size < 5 || level->block_size > 24 ||
            !within(level->logical_offset, level->hash_data_size, size))
            return -2;
        uint64_t block_size = UINT64_C(1) << level->block_size;
        uint64_t blocks =
            level->hash_data_size / block_size + (level->hash_data_size % block_size != 0);
        if (blocks > (UINT64_MAX - total) / block_size)
            return -2;
        total += blocks * block_size;
        uint64_t hash_size = i ? ivfc->level_headers[i - 1].hash_data_size : 32;
        if (blocks > hash_size / 32)
            return -2;
        for (unsigned j = 0; j < i; j++) {
            const ivfc_level_hdr_t *other = &ivfc->level_headers[j];
            if (level->logical_offset < other->logical_offset + other->hash_data_size &&
                other->logical_offset < level->logical_offset + level->hash_data_size)
                return -2;
        }
    }
    *out_total = total;
    return 0;
}
static int ivfc_verify(ivfc_reader_t reader, void *context, const ivfc_hdr_t *ivfc, uint64_t size,
                       int progress_phase) {
    uint64_t total;
    int validation = ivfc_validate(ivfc, size, &total);
    if (validation)
        return validation;
    unsigned char *chunk = malloc(CHUNK_LIMIT);
    if (!chunk)
        return -1;
    int rc = 0;
    uint64_t verified = 0, next_report = 64u * 1024u * 1024u;
    if (progress_phase)
        nsp_ivfc_report(progress_phase, 0, (double)total);
    for (unsigned i = 0; !rc && i < IVFC_MAX_LEVEL; i++) {
        const ivfc_level_hdr_t *level = &ivfc->level_headers[i];
        uint64_t block_size = UINT64_C(1) << level->block_size;
        uint64_t blocks =
            level->hash_data_size / block_size + (level->hash_data_size % block_size != 0);
        for (uint64_t b = 0; !rc && b < blocks; b++) {
            unsigned char expected[32], digest[32];
            if (!i)
                memcpy(expected, ivfc->master_hash, 32);
            else
                rc = reader(context, ivfc->level_headers[i - 1].logical_offset + b * 32, expected,
                            32);
            if (rc)
                break;
            uint64_t data_size = level->hash_data_size - b * block_size;
            if (data_size > block_size)
                data_size = block_size;
            sha_ctx_t *hash = new_sha_ctx(HASH_TYPE_SHA256, 0);
            for (uint64_t done = 0; !rc && done < block_size;) {
                size_t count =
                    block_size - done > CHUNK_LIMIT ? CHUNK_LIMIT : (size_t)(block_size - done);
                memset(chunk, 0, count);
                if (done < data_size) {
                    size_t take = data_size - done < count ? (size_t)(data_size - done) : count;
                    rc =
                        reader(context, level->logical_offset + b * block_size + done, chunk, take);
                }
                if (!rc)
                    sha_update(hash, chunk, count);
                done += count;
            }
            if (!rc) {
                sha_get_hash(hash, digest);
                if (memcmp(digest, expected, 32))
                    rc = -3;
                else {
                    verified += block_size;
                    if (progress_phase && verified >= next_report) {
                        nsp_ivfc_report(progress_phase, (double)verified, (double)total);
                        next_report = verified + 64u * 1024u * 1024u;
                    }
                }
            }
            free_sha_ctx(hash);
        }
    }
    nsp_zero(chunk, CHUNK_LIMIT);
    free(chunk);
    if (!rc && progress_phase)
        nsp_ivfc_report(progress_phase, (double)verified, (double)total);
    return rc;
}
static int ivfc_section_read(void *context, uint64_t offset, void *out, size_t size) {
    (void)context;
    return read_at(offset, out, size);
}
static int read_patch_physical(uint64_t offset, unsigned char *out, size_t size) {
    if (!within(offset, size, section->size) || size > CHUNK_LIMIT || !subsection_count)
        return -2;
    unsigned char *buffer = malloc(CHUNK_LIMIT + 16);
    if (!buffer)
        return -1;
    int rc = 0;
    while (size && !rc) {
        subsection_t *sub = &subsections[subsection_index(offset)];
        if (offset < sub->start || offset >= sub->end) {
            rc = -2;
            break;
        }
        size_t count = sub->end - offset < size ? (size_t)(sub->end - offset) : size;
        uint64_t absolute = section->offset + offset;
        size_t skip = (size_t)(absolute & 15), take = count + skip;
        if (!within(offset - skip, take, section->size) ||
            fseeko(section->file, (off_t)(absolute - skip), SEEK_SET) ||
            fread(buffer, 1, take, section->file) != take) {
            rc = -5;
            break;
        }
        if (!section->is_decrypted) {
            unsigned char counter[16];
            bktr_counter(counter, section->header->section_ctr, sub->generation, absolute);
            aes_setiv(section->aes, counter, 16);
            aes_decrypt(section->aes, buffer, buffer, take);
            nsp_zero(counter, sizeof counter);
        }
        memcpy(out, buffer + skip, count);
        offset += count;
        out += count;
        size -= count;
    }
    nsp_zero(buffer, CHUNK_LIMIT + 16);
    free(buffer);
    return rc;
}
static int read_merged(uint64_t offset, void *output, size_t size) {
    if (!within(offset, size, merged_size) || size > CHUNK_LIMIT || !relocation_count ||
        !base_section)
        return -2;
    unsigned char *out = output;
    while (size) {
        relocation_t *entry = &relocations[relocation_index(offset)];
        if (offset < entry->start || offset >= entry->end)
            return -2;
        size_t count = entry->end - offset < size ? (size_t)(entry->end - offset) : size;
        uint64_t physical = entry->physical + offset - entry->start;
        int rc = 0;
        if (entry->source)
            rc = read_patch_physical(physical, out, count);
        else {
            if (!within(physical, count, base_section->size))
                return -2;
            nca_section_fseek(base_section, physical);
            if (nca_section_fread(base_section, out, count) != count)
                rc = -5;
        }
        if (rc)
            return rc;
        offset += count;
        out += count;
        size -= count;
    }
    return 0;
}

static int open_romfs(const char *path, unsigned char *out_size, int verify) {
    int rc = open_authenticated_section(path, 2, 0);
    if (rc) {
        nsp_close();
        return rc;
    }
    ivfc_hdr_t *ivfc = &section->header->romfs_superblock.ivfc_header;
    /* 旧版非 BKTR RomFS 不使用补丁信息的剩余部分。
     * Legacy non-BKTR RomFS leaves the patch-info remainder unused. */
    for (size_t i = 0; i < sizeof section->header->romfs_superblock._0xE0; i++)
        if (section->header->romfs_superblock._0xE0[i]) {
            nsp_close();
            return -4;
        }
    uint64_t total;
    rc = verify ? ivfc_verify(ivfc_section_read, NULL, ivfc, extent, 1)
                : ivfc_validate(ivfc, extent, &total);
    if (!rc) {
        const ivfc_level_hdr_t *last = &ivfc->level_headers[IVFC_MAX_LEVEL - 1];
        romfs_hdr_t header;
        if (last->hash_data_size < sizeof header)
            rc = -2;
        else
            rc = read_at(last->logical_offset, &header, sizeof header);
        if (!rc && header.header_size != ROMFS_HEADER_SIZE)
            rc = -2;
        if (!rc) {
            romfs_offset = last->logical_offset;
            romfs_size = last->hash_data_size;
            memcpy(out_size, &romfs_size, 8);
        }
    }
    if (rc)
        nsp_close();
    return rc;
}
int nsp_open_romfs(const char *path, unsigned char *out_size) {
    return open_romfs(path, out_size, 1);
}
/* 此入口跳过完整 IVFC 扫描。默认调用方已将完整 NCA 摘要与 CNMT 比对；
 * 显式快速模式则接受未经完整验证的内容。独立调用方仍可使用严格入口。
 * This entrypoint skips the full IVFC scan. Default callers have checked the
 * whole-NCA digest against CNMT; explicit quick mode accepts unverified content.
 * Independent callers can use the strict entrypoint. */
int nsp_open_romfs_hash_verified(const char *path, unsigned char *out_size) {
    return open_romfs(path, out_size, 0);
}
/* 严格入口分别认证基础与合并后的 IVFC。快速入口跳过这两次扫描；
 * 调用方必须明确决定是否接受未经完整验证的内容。
 * The strict entrypoint authenticates base and merged IVFC independently. The
 * fast entrypoint skips both scans; callers decide whether unverified content
 * is acceptable.
 */
static int open_patched_romfs(const char *base_path, const char *patch_path,
                              unsigned char *out_size, int verify) {
    unsigned char ignored_size[8];
    int rc = open_romfs(base_path, ignored_size, verify);
    if (rc)
        return rc;
    nca_ctx_t *saved = malloc(sizeof nca);
    if (!saved) {
        nsp_close();
        return -1;
    }
    unsigned index = (unsigned)(section - nca.section_contexts);
    memcpy(saved, &nca, sizeof nca);
    saved->section_contexts[index].header = &saved->header.fs_headers[index];
    /* 在打开另一个 NCA（这会关闭当前活动状态）之前转移所有权。
     * Transfer ownership before opening another NCA (which closes active state).
     */
    nsp_zero(&nca, sizeof nca);
    section = NULL;
    extent = 0;
    romfs_offset = romfs_size = 0;
    rc = open_authenticated_section(patch_path, 3, 0);
    base_nca = saved;
    base_section = &saved->section_contexts[index];
    if (rc) {
        nsp_close();
        return rc;
    }
    bktr_superblock_t *sb = &section->header->bktr_superblock;
    for (size_t i = 0; i < sizeof sb->_0xE0; i++)
        if (sb->_0xE0[i]) {
            rc = -4;
            break;
        }
    bktr_header_t *rh = &sb->relocation_header, *sh = &sb->subsection_header;
    if (!rc && (rh->magic != MAGIC_BKTR || sh->magic != MAGIC_BKTR || rh->_0x14 != 1 ||
                sh->_0x14 != 1 || rh->_0x1C || sh->_0x1C || !rh->offset || rh->offset % 16 ||
                sh->offset % 16 || rh->size > META_LIMIT || sh->size > META_LIMIT ||
                !within(rh->offset, rh->size, section->size) ||
                !within(sh->offset, sh->size, section->size) ||
                rh->offset + rh->size != sh->offset || sh->offset + sh->size != section->size))
        rc = -4;
    for (unsigned which = 0; which < 2 && !rc; which++) {
        bktr_header_t *header = which ? sh : rh;
        unsigned char *bytes = malloc((size_t)header->size);
        if (!bytes) {
            rc = -1;
            break;
        }
        for (uint64_t offset = 0; offset < header->size && !rc;) {
            size_t count =
                header->size - offset > CHUNK_LIMIT ? CHUNK_LIMIT : (size_t)(header->size - offset);
            rc = read_at(header->offset + offset, bytes + offset, count);
            offset += count;
        }
        if (!rc)
            rc = parse_bktr_table(bytes, header->size, header->num_entries, which,
                                  base_section->size, rh->offset, sh->offset);
        nsp_zero(bytes, (size_t)header->size);
        free(bytes);
    }
    if (!rc) {
        merged_active = 1;
        extent = merged_size;
        uint64_t total;
        rc = verify ? ivfc_verify(ivfc_section_read, NULL, &sb->ivfc_header, extent, 2)
                    : ivfc_validate(&sb->ivfc_header, extent, &total);
    }
    if (!rc) {
        const ivfc_level_hdr_t *last = &sb->ivfc_header.level_headers[IVFC_MAX_LEVEL - 1];
        romfs_hdr_t header;
        if (last->hash_data_size < sizeof header)
            rc = -2;
        else
            rc = read_at(last->logical_offset, &header, sizeof header);
        if (!rc && header.header_size != ROMFS_HEADER_SIZE)
            rc = -2;
        if (!rc) {
            romfs_offset = last->logical_offset;
            romfs_size = last->hash_data_size;
            memcpy(out_size, &romfs_size, 8);
        }
    }
    if (rc)
        nsp_close();
    return rc;
}
int nsp_open_patched_romfs(const char *base_path, const char *patch_path, unsigned char *out_size) {
    return open_patched_romfs(base_path, patch_path, out_size, 1);
}
int nsp_open_patched_romfs_hash_verified(const char *base_path, const char *patch_path,
                                         unsigned char *out_size) {
    return open_patched_romfs(base_path, patch_path, out_size, 0);
}

int nsp_read_romfs(uint32_t lo, uint32_t hi, unsigned char *out, uint32_t count) {
    uint64_t offset = pair(lo, hi);
    if (!romfs_size || count > CHUNK_LIMIT || !within(offset, count, romfs_size))
        return -2;
    int rc = read_at(romfs_offset + offset, out, count);
    return rc ? rc : (int)count;
}
/* 封闭的合成夹具：无调用方可控的根、无持久读取器状态，
 * 也不存在绕过生产 RSA/IVFC 入口的路径。
 * Closed synthetic fixture: no caller-controlled root, no persistent reader
 * state and no path around the production RSA/IVFC entrypoint. */
static int ivfc_fixture_read(void *context, uint64_t offset, void *out, size_t size) {
    if (!within(offset, size, 384))
        return -2;
    memcpy(out, (unsigned char *)context + offset, size);
    return 0;
}
int nsp_ivfc_fixture(void) {
    unsigned char bytes[384] = {0};
    memcpy(bytes + 5 * 64, "abc", 3);
    for (int i = 4; i >= 0; i--)
        sha256_hash_buffer(bytes + i * 64, bytes + (i + 1) * 64, 64);
    ivfc_hdr_t header = {0};
    header.magic = MAGIC_IVFC;
    header.id = 0x20000;
    header.num_levels = 7;
    header.master_hash_size = 32;
    for (unsigned i = 0; i < 6; i++) {
        header.level_headers[i].logical_offset = i * 64;
        header.level_headers[i].hash_data_size = i == 5 ? 3 : 32;
        header.level_headers[i].block_size = 6;
    }
    /* 使用 Python hashlib 独立生成：从公开 ASCII 字符串 abc 开始，
     * 重复执行 SHA256 并填充到 64 字节，共六次。
     * Independently generated with Python hashlib: repeatedly SHA256-pad-to64
     * six times starting from the public ASCII string abc. */
    const unsigned char root[32] = {0xa4, 0xed, 0xb8, 0x73, 0x6e, 0xb0, 0xc7, 0x4c,
                                    0x95, 0x13, 0xda, 0xfc, 0x57, 0x55, 0xd7, 0xc0,
                                    0x5f, 0x50, 0x05, 0x63, 0x3b, 0x42, 0xc0, 0x0b,
                                    0xff, 0x86, 0x3d, 0x95, 0x7d, 0xa1, 0x26, 0x7e};
    memcpy(header.master_hash, root, 32);
    if (ivfc_verify(ivfc_fixture_read, bytes, &header, sizeof bytes, 0))
        return 1;
    bytes[320] ^= 1;
    if (ivfc_verify(ivfc_fixture_read, bytes, &header, sizeof bytes, 0) != -3)
        return 2;
    bytes[320] ^= 1;
    header.level_headers[1].logical_offset = 0;
    if (ivfc_verify(ivfc_fixture_read, bytes, &header, sizeof bytes, 0) != -2)
        return 3;
    header.level_headers[1].logical_offset = 64;
    header.level_headers[5].block_size = 32;
    if (ivfc_verify(ivfc_fixture_read, bytes, &header, sizeof bytes, 0) != -2)
        return 4;
    return 0;
}
/* 封闭的公开合成夹具。不接受路径、密钥、根或回调，
 * 总在返回前销毁临时上下文，且不能打开内容。
 * Closed public synthetic fixture. Never accepts a path, key, root or callback,
 * and always destroys temporary contexts before returning. Cannot open content.
 */
int nsp_bktr_fixture(void) {
    if (section || raw || base_nca || nca.file)
        return -1;
    int result = 1;
    unsigned char *table = calloc(1, 0x8000);
    if (!table)
        return -1;
    unsigned char base[384] = {0}, plain[128] = {0}, cipher[128], key[16], digest[32];
    memcpy(base + 320, "abc", 3);
    memcpy(plain + 48, "abc", 3);
    for (int i = 4; i >= 0; i--)
        sha256_hash_buffer(base + i * 64, base + (i + 1) * 64, 64);
    for (unsigned i = 0; i < 16; i++)
        key[i] = (unsigned char)i;
    const unsigned char counter_source[8] = {0, 0, 0, 0, 0x44, 0x33, 0x22, 0x11};
    const unsigned char expected_cipher_hash[32] = {0x1e, 0xc0, 0xdf, 0xfa, 0x35, 0x5f, 0x0a, 0xb3,
                                                    0x57, 0xb3, 0x1a, 0xd6, 0x54, 0xc2, 0x0b, 0x5b,
                                                    0x40, 0xf1, 0xde, 0x13, 0x16, 0x8a, 0xd7, 0xc5,
                                                    0xd9, 0x2d, 0x6e, 0xe9, 0x4c, 0x03, 0x28, 0x0d};
    aes_ctx_t *aes = new_aes_ctx(key, 16, AES_MODE_CTR);
    for (unsigned i = 0; i < 2; i++) {
        unsigned char counter[16];
        bktr_counter(counter, counter_source, 7 + i, 0x200 + i * 64);
        aes_setiv(aes, counter, 16);
        aes_encrypt(aes, cipher + i * 64, plain + i * 64, 64);
    }
    free_aes_ctx(aes);
    /* 独立的 OpenSSL AES-128-CTR 密文摘要，而非同一路径的往返校验。
     * Independent OpenSSL AES-128-CTR ciphertext digest, not same-path roundtrip.
     */
    sha256_hash_buffer(digest, cipher, sizeof cipher);
    if (memcmp(digest, expected_cipher_hash, 32))
        goto done;
    uint32_t one = 1, two = 2;
    uint64_t total = 384, start = 320, physical = 48;
    memcpy(table + 4, &one, 4);
    memcpy(table + 8, &total, 8);
    memcpy(table + 0x4004, &two, 4);
    memcpy(table + 0x4008, &total, 8);
    memcpy(table + 0x4024, &start, 8);
    memcpy(table + 0x402c, &physical, 8);
    memcpy(table + 0x4034, &two, 4);
    if (parse_bktr_table(table, 0x8000, 2, 0, 384, 128, 0x800) != -2)
        goto done;
    memcpy(table + 0x4034, &one, 4);
    if (parse_bktr_table(table, 0x8000, 2, 0, 384, 128, 0x800))
        goto done;
    memset(table, 0, 0x8000);
    total = 0x800;
    start = 64;
    memcpy(table + 4, &one, 4);
    memcpy(table + 8, &total, 8);
    memcpy(table + 0x4004, &two, 4);
    memcpy(table + 0x4008, &total, 8);
    memcpy(table + 0x4020, &start, 8);
    uint32_t generation = 7;
    memcpy(table + 0x401c, &generation, 4);
    generation = 8;
    memcpy(table + 0x402c, &generation, 4);
    memcpy(table + 0x4018, &one, 4);
    if (parse_bktr_table(table, 0x8000, 2, 1, 384, 128, 0x800) != -4)
        goto done;
    memset(table + 0x4018, 0, 4);
    if (parse_bktr_table(table, 0x8000, 2, 1, 384, 128, 0x800))
        goto done;
    base_nca = calloc(1, sizeof *base_nca);
    if (!base_nca)
        goto done;
    base_section = &base_nca->section_contexts[0];
    base_nca->file = tmpfile();
    nca.file = tmpfile();
    if (!base_nca->file || !nca.file)
        goto done;
    if (fwrite(base, 1, sizeof base, base_nca->file) != sizeof base)
        goto done;
    unsigned char zero[0x200] = {0};
    if (fwrite(zero, 1, sizeof zero, nca.file) != sizeof zero ||
        fwrite(cipher, 1, sizeof cipher, nca.file) != sizeof cipher)
        goto done;
    base_section->file = base_nca->file;
    base_section->size = sizeof base;
    base_section->is_decrypted = 1;
    section = &nca.section_contexts[0];
    section->file = nca.file;
    section->offset = 0x200;
    section->size = 128;
    section->header = &nca.header.fs_headers[0];
    memcpy(section->header->section_ctr, counter_source, 8);
    section->aes = new_aes_ctx(key, 16, AES_MODE_CTR);
    merged_active = 1;
    extent = 384;
    unsigned char output[69];
    if (read_at(315, output, sizeof output) || memcmp(output, base + 315, 69))
        goto done;
    if (read_at(321, output, 2) || memcmp(output, "bc", 2))
        goto done;
    ivfc_hdr_t header = {0};
    header.magic = MAGIC_IVFC;
    header.id = 0x20000;
    header.num_levels = 7;
    header.master_hash_size = 32;
    for (unsigned i = 0; i < 6; i++) {
        header.level_headers[i].logical_offset = i * 64;
        header.level_headers[i].hash_data_size = i == 5 ? 3 : 32;
        header.level_headers[i].block_size = 6;
    }
    sha256_hash_buffer(header.master_hash, base, 64);
    if (ivfc_verify(ivfc_section_read, NULL, &header, 384, 0))
        goto done;
    cipher[48] ^= 1;
    if (fseeko(nca.file, 0x200, SEEK_SET) ||
        fwrite(cipher, 1, sizeof cipher, nca.file) != sizeof cipher)
        goto done;
    if (ivfc_verify(ivfc_section_read, NULL, &header, 384, 0) != -3)
        goto done;
    cipher[48] ^= 1;
    if (fseeko(nca.file, 0x200, SEEK_SET) ||
        fwrite(cipher, 1, sizeof cipher, nca.file) != sizeof cipher)
        goto done;
    base[0] ^= 1;
    if (fseeko(base_nca->file, 0, SEEK_SET) ||
        fwrite(base, 1, sizeof base, base_nca->file) != sizeof base)
        goto done;
    if (ivfc_verify(ivfc_section_read, NULL, &header, 384, 0) != -3)
        goto done;
    result = 0;
done:
    nsp_zero(table, 0x8000);
    free(table);
    nsp_zero(key, sizeof key);
    nsp_close();
    return result;
}

int nsp_open_meta(const char *path) {
    int rc = open_authenticated_pfs0(path, 0, 0);
    if (rc)
        nsp_close();
    return rc;
}
int nsp_open_warlock_meta(const char *path) {
    int rc = open_authenticated_pfs0(path, 0, 1);
    if (rc)
        nsp_close();
    return rc;
}
int nsp_open_exefs(const char *path) {
    int rc = open_authenticated_pfs0(path, 1, 0);
    if (rc)
        nsp_close();
    return rc;
}
int nsp_hash_input(const char *path, unsigned char *out) {
    nsp_close();
    raw = fopen(path, "rb");
    if (!raw)
        return -5;
    if (fseeko(raw, 0, SEEK_END)) {
        nsp_close();
        return -5;
    }
    off_t size = ftello(raw);
    if (size < 0) {
        nsp_close();
        return -5;
    }
    extent = (uint64_t)size;
    int rc = hash_range(0, extent, out);
    nsp_close();
    return rc;
}
int nsp_hash_entry(uint32_t index, unsigned char *out) {
    if (!directory || index >= directory->num_files)
        return -1;
    pfs0_file_entry_t *entry = pfs0_get_file_entry(directory, index);
    return hash_range(pfs_offset + directory_size + entry->offset, entry->size, out);
}

int nsp_entry_count(void) { return directory ? (int)directory->num_files : -1; }
int nsp_entry_info(uint32_t index, unsigned char *out, uint32_t capacity) {
    if (!directory || index >= directory->num_files || capacity < 17)
        return -1;
    pfs0_file_entry_t *e = pfs0_get_file_entry(directory, index);
    const char *name = pfs0_get_file_name(directory, index);
    size_t len = strlen(name) + 1;
    if (len > capacity - 16)
        return -2;
    memcpy(out, &e->offset, 8);
    memcpy(out + 8, &e->size, 8);
    memcpy(out + 16, name, len);
    return (int)(16 + len);
}
int nsp_read_entry(uint32_t index, uint32_t lo, uint32_t hi, unsigned char *out, uint32_t count) {
    if (!directory || index >= directory->num_files || count > CHUNK_LIMIT)
        return -1;
    pfs0_file_entry_t *e = pfs0_get_file_entry(directory, index);
    uint64_t offset = pair(lo, hi);
    if (!within(offset, count, e->size))
        return -2;
    int rc = read_at(pfs_offset + directory_size + e->offset + offset, out, count);
    return rc ? rc : (int)count;
}
int nsp_crypto_fixture(void);
