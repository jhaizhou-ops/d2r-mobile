// D2R 浏览器修改：保留分支基于 fopen 的映射，限制分配并拒绝短读和定位失败。
// 资源哈希不进入此操作。
// D2R browser modification: retain the fork's fopen-backed mapping, but bound it
// and reject short reads/seeks. No resource hashing reaches this operation.
void *ZFile::MapFile(const char *path, size_t offset, size_t size, size_t *psize, bool ro) {
    if (psize)
        *psize = 0;
    struct stat info = {};
    if (!path || stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return NULL;
    const uint64_t fileSize = (uint64_t)info.st_size;
    if (offset > fileSize)
        return NULL;
    if (!size) {
        if (fileSize - offset > 134217728)
            return NULL;
        size = (size_t)(fileSize - offset);
    }
    if (!size || size > 134217728 || (uint64_t)size > fileSize - offset)
        return NULL;
    FILE *file = fopen(path, ro ? "rb" : "rb+");
    if (!file)
        return NULL;
    uint8_t *bytes = (uint8_t *)malloc(size);
    bool ok = bytes && fseeko(file, (off_t)offset, SEEK_SET) == 0;
    size_t read = 0;
    while (ok && read < size) {
        size_t count = fread(bytes + read, 1, min((size_t)65536, size - read), file);
        if (!count) {
            ok = false;
            break;
        }
        read += count;
    }
    ok = !ferror(file) && ok;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        free(bytes);
        return NULL;
    }
    s_mapFilesNoMmap[bytes] = {path, offset, size, ro};
    if (psize)
        *psize = size;
    return bytes;
}

bool ZFile::UnmapFile(void *base, size_t size) {
    auto found = s_mapFilesNoMmap.find(base);
    if (found == s_mapFilesNoMmap.end())
        return false;
    const ZFileMappedInfo info = found->second;
    bool ok = size == info.size;
    if (ok && !info.ro) {
        FILE *file = fopen(info.path.c_str(), "rb+");
        if (!file)
            ok = false;
        else {
            ok = fseeko(file, (off_t)info.offset, SEEK_SET) == 0;
            size_t written = 0;
            while (ok && written < info.size) {
                size_t count = fwrite((uint8_t *)base + written, 1,
                                      min((size_t)65536, info.size - written), file);
                if (!count) {
                    ok = false;
                    break;
                }
                written += count;
            }
            ok = fflush(file) == 0 && ok;
            ok = fclose(file) == 0 && ok;
        }
    }
    free(base);
    s_mapFilesNoMmap.erase(found);
    return ok;
}
