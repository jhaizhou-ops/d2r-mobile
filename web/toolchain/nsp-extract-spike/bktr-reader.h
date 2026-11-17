/* 严格的旧版单根 BKTR 表，不使用上游原地哨兵解析器。表内容仍不可信，只有合并 IVFC
 * 验证才能认证返回明文；这是 boundary.c 的私有实现。 */
/* Strict legacy single-root BKTR tables. No upstream in-place sentinel parser.
 * Table contents remain untrusted; only merged IVFC verification authenticates
 * the returned plaintext. Private implementation of boundary.c. */
typedef struct {
    uint64_t start, end, physical;
    uint32_t source;
} relocation_t;
typedef struct {
    uint64_t start, end;
    uint32_t generation;
} subsection_t;
static relocation_t *relocations;
static subsection_t *subsections;
static uint32_t relocation_count, subsection_count;
static uint64_t merged_size;
static int merged_active;
static nca_ctx_t *base_nca;
static nca_section_ctx_t *base_section;
static void close_patch(void) {
    free(relocations);
    free(subsections);
    relocations = NULL;
    subsections = NULL;
    relocation_count = subsection_count = 0;
    merged_size = 0;
    merged_active = 0;
    if (base_section && base_section->aes)
        free_aes_ctx(base_section->aes);
    if (base_nca) {
        if (base_nca->file)
            fclose(base_nca->file);
        nsp_zero(base_nca, sizeof *base_nca);
        free(base_nca);
    }
    base_nca = NULL;
    base_section = NULL;
}
/* 复制线格式字段，不通过未对齐的紧凑指针访问。 */
/* Wire fields are copied, never accessed through unaligned packed pointers. */
static uint64_t get64(const unsigned char *p) {
    uint64_t n;
    memcpy(&n, p, 8);
    return n;
}
static uint32_t get32(const unsigned char *p) {
    uint32_t n;
    memcpy(&n, p, 4);
    return n;
}
static int parse_bktr_table(const unsigned char *data, uint64_t bytes, uint32_t expected,
                            int is_sub, uint64_t base_limit, uint64_t patch_limit,
                            uint64_t subsection_offset) {
    if (bytes < 0x8000 || bytes > META_LIMIT || bytes % 0x4000 || !expected)
        return -2;
    uint32_t buckets = get32(data + 4);
    uint64_t total = get64(data + 8);
    if (!buckets || buckets > 0x3ff0 / 8 || bytes != ((uint64_t)buckets + 1) * 0x4000 || !total ||
        expected > (uint64_t)buckets * (0x3ff0 / (is_sub ? 16 : 20)))
        return -2;
    if (is_sub && total != subsection_offset)
        return -2;
    void *records = calloc(expected, is_sub ? sizeof(subsection_t) : sizeof(relocation_t));
    if (!records)
        return -1;
    uint32_t used = 0;
    int rc = 0;
    for (uint32_t b = 0; b < buckets && !rc; b++) {
        uint64_t start = get64(data + 16 + b * 8);
        uint64_t end = b + 1 < buckets ? get64(data + 24 + b * 8) : total;
        const unsigned char *node = data + ((uint64_t)b + 1) * 0x4000;
        uint32_t count = get32(node + 4), stride = is_sub ? 16 : 20;
        if (!count || count > 0x3ff0 / stride || start >= end || end > total || (!b && start) ||
            get64(node + 8) != end || count > expected - used) {
            rc = -2;
            break;
        }
        for (uint32_t i = 0; i < count; i++) {
            const unsigned char *entry = node + 16 + i * stride;
            uint64_t at = get64(entry), next = i + 1 < count ? get64(entry + stride) : end;
            if ((!i && at != start) || at < start || at >= next || next > end) {
                rc = -2;
                break;
            }
            if (is_sub) {
                /* 只支持旧版加密记录，明确不支持现代 NotEncrypted 或非零保留字节。 */
                /* Only legacy encrypted records. Modern NotEncrypted and
                 * nonzero reserved bytes are explicitly unsupported. */
                if (get32(entry + 8) || at >= patch_limit || at % 16) {
                    rc = -4;
                    break;
                }
                if (next > patch_limit) {
                    if (b + 1 != buckets || i + 1 != count) {
                        rc = -2;
                        break;
                    }
                    next = patch_limit;
                }
                if (at >= next) {
                    rc = -2;
                    break;
                }
                ((subsection_t *)records)[used++] = (subsection_t){at, next, get32(entry + 12)};
            } else {
                uint64_t physical = get64(entry + 8);
                uint32_t source = get32(entry + 16);
                uint64_t limit = source ? patch_limit : base_limit;
                if (source > 1 || !within(physical, next - at, limit)) {
                    rc = -2;
                    break;
                }
                ((relocation_t *)records)[used++] = (relocation_t){at, next, physical, source};
            }
        }
    }
    if (!rc && used != expected)
        rc = -2;
    if (rc) {
        free(records);
        return rc;
    }
    if (is_sub) {
        subsections = records;
        subsection_count = used;
    } else {
        relocations = records;
        relocation_count = used;
        merged_size = total;
    }
    return 0;
}
static uint32_t relocation_index(uint64_t offset) {
    uint32_t low = 0, high = relocation_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (relocations[mid].start <= offset)
            low = mid + 1;
        else
            high = mid;
    }
    return low ? low - 1 : 0;
}
static uint32_t subsection_index(uint64_t offset) {
    uint32_t low = 0, high = subsection_count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2;
        if (subsections[mid].start <= offset)
            low = mid + 1;
        else
            high = mid;
    }
    return low ? low - 1 : 0;
}
static void bktr_counter(unsigned char out[16], const unsigned char section_counter[8],
                         uint32_t generation, uint64_t absolute) {
    for (unsigned i = 0; i < 4; i++)
        out[i] = section_counter[7 - i];
    for (unsigned i = 0; i < 4; i++)
        out[7 - i] = (unsigned char)(generation >> (i * 8));
    absolute >>= 4;
    for (unsigned i = 0; i < 8; i++) {
        out[15 - i] = (unsigned char)absolute;
        absolute >>= 8;
    }
}
