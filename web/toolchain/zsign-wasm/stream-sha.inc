// D2R 浏览器修改：有界读取资源并计算哈希，不对资源进行内存映射。
// D2R browser modification: bounded resource hashing, never mmap a resource.
bool ZSHA::SHAFile(const char *path, string &sha1, string &sha256) {
    sha1.clear();
    sha256.clear();
    struct stat info = {};
    if (!path || stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0)
        return false;
    FILE *file = fopen(path, "rb");
    if (!file)
        return false;
    EVP_MD_CTX *first = EVP_MD_CTX_new();
    EVP_MD_CTX *second = EVP_MD_CTX_new();
    bool ok = first && second && EVP_DigestInit_ex(first, EVP_sha1(), NULL) == 1 &&
              EVP_DigestInit_ex(second, EVP_sha256(), NULL) == 1;
    unsigned char bytes[65536];
    uint64_t total = 0;
    while (ok) {
        size_t count = fread(bytes, 1, sizeof(bytes), file);
        if (count) {
            total += count;
            ok = total <= (uint64_t)info.st_size && EVP_DigestUpdate(first, bytes, count) == 1 &&
                 EVP_DigestUpdate(second, bytes, count) == 1;
        }
        if (count != sizeof(bytes)) {
            ok = ok && !ferror(file) && feof(file);
            break;
        }
    }
    ok = fclose(file) == 0 && ok && total == (uint64_t)info.st_size;
    unsigned char a[EVP_MAX_MD_SIZE], b[EVP_MAX_MD_SIZE];
    unsigned int alen = 0, blen = 0;
    if (ok)
        ok = EVP_DigestFinal_ex(first, a, &alen) == 1 && alen == 20 &&
             EVP_DigestFinal_ex(second, b, &blen) == 1 && blen == 32;
    if (ok) {
        sha1.assign((const char *)a, alen);
        sha256.assign((const char *)b, blen);
    }
    OPENSSL_cleanse(bytes, sizeof(bytes));
    EVP_MD_CTX_free(first);
    EVP_MD_CTX_free(second);
    return ok;
}
