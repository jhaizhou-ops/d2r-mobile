// 离线兼容性门禁，不验证 Apple 信任链、撤销状态或设备安装资格。
// Offline compatibility gate, not Apple trust-chain, revocation or device-install verification.
// 包含于 openssl.cpp；openssl.h 声明 ValidateProfileForBundle(const string&, bool* = nullptr)。
// Include in openssl.cpp; declare ValidateProfileForBundle(const string&, bool* = nullptr) in
// openssl.h.
#include <ctime>

namespace {
constexpr size_t D2R_PROFILE_BYTES = 4 * 1024 * 1024;

bool D2RIdentifier(const string &value, bool dotted) {
    if (value.empty() || value.size() > (dotted ? 255U : 64U))
        return false;
    bool segment = false;
    for (unsigned char ch : value) {
        if (dotted && ch == '.') {
            if (!segment)
                return false;
            segment = false;
        } else {
            if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                  (ch >= '0' && ch <= '9') || (dotted && ch == '-')))
                return false;
            segment = true;
        }
    }
    return segment;
}

bool D2RMatchApplication(const string &pattern, const string &concrete) {
    if (!D2RIdentifier(concrete, true) || pattern.empty() || pattern.size() > 255)
        return false;
    size_t star = pattern.find('*');
    if (star == string::npos)
        return D2RIdentifier(pattern, true) && pattern == concrete;
    // 仅接受完整末尾段通配符，且必须匹配至少一个非空字符。
    // Only a complete trailing-segment wildcard is supported, matching a nonempty suffix.
    if (star != pattern.size() - 1 || star < 2 || pattern[star - 1] != '.' ||
        !D2RIdentifier(pattern.substr(0, star - 1), true))
        return false;
    return concrete.size() > star && concrete.compare(0, star, pattern, 0, star) == 0;
}

// 独立读取标准 XML UTC 日期，避免上游 plist 日期解析中的本地时区转换。
// Independently read standard XML UTC dates, avoiding upstream plist local-time conversion.
ASN1_TIME *D2RProfileDate(const string &xml, const char *key) {
    string marker = string("<key>") + key + "</key>";
    size_t at = xml.find(marker);
    if (at == string::npos || xml.find(marker, at + marker.size()) != string::npos)
        return nullptr;
    at += marker.size();
    while (at < xml.size() &&
           (xml[at] == ' ' || xml[at] == '\t' || xml[at] == '\r' || xml[at] == '\n'))
        ++at;
    if (xml.compare(at, 6, "<date>") != 0 || at + 33 > xml.size())
        return nullptr;
    string date = xml.substr(at + 6, 20);
    if (xml.compare(at + 26, 7, "</date>") != 0 || date[4] != '-' || date[7] != '-' ||
        date[10] != 'T' || date[13] != ':' || date[16] != ':' || date[19] != 'Z')
        return nullptr;
    string generalized;
    for (size_t i = 0; i < 19; ++i) {
        if (i == 4 || i == 7 || i == 10 || i == 13 || i == 16)
            continue;
        if (date[i] < '0' || date[i] > '9')
            return nullptr;
        generalized.push_back(date[i]);
    }
    generalized.push_back('Z');
    ASN1_TIME *value = ASN1_TIME_new();
    if (!value || ASN1_TIME_set_string_X509(value, generalized.c_str()) != 1) {
        ASN1_TIME_free(value);
        return nullptr;
    }
    return value;
}

bool D2RProfileDates(const string &xml, X509 *cert, time_t now) {
    if (!cert || now <= 0)
        return false;
    ASN1_TIME *current = ASN1_TIME_set(nullptr, now);
    ASN1_TIME *created = D2RProfileDate(xml, "CreationDate");
    ASN1_TIME *expires = D2RProfileDate(xml, "ExpirationDate");
    // ASN1_TIME_compare 的 -2 表示解析失败，不能当作有效的先后关系。
    // ASN1_TIME_compare returns -2 on parse error, not a valid ordering.
    int before = current ? ASN1_TIME_compare(X509_get0_notBefore(cert), current) : -2;
    int after = current ? ASN1_TIME_compare(X509_get0_notAfter(cert), current) : -2;
    int creation = current && created ? ASN1_TIME_compare(created, current) : -2;
    int expiration = current && expires ? ASN1_TIME_compare(expires, current) : -2;
    bool valid = (before == -1 || before == 0) && after == 1 && (creation == -1 || creation == 0) &&
                 expiration == 1;
    ASN1_TIME_free(current);
    ASN1_TIME_free(created);
    ASN1_TIME_free(expires);
    return valid;
}

bool D2RCertificateTeam(X509 *cert, const string &team) {
    if (!cert || !D2RIdentifier(team, false))
        return false;
    X509_NAME *name = X509_get_subject_name(cert);
    int index = name ? X509_NAME_get_index_by_NID(name, NID_organizationalUnitName, -1) : -1;
    if (index < 0 || X509_NAME_get_index_by_NID(name, NID_organizationalUnitName, index) >= 0)
        return false;
    X509_NAME_ENTRY *entry = X509_NAME_get_entry(name, index);
    unsigned char *utf8 = nullptr;
    int length = ASN1_STRING_to_UTF8(&utf8, X509_NAME_ENTRY_get_data(entry));
    bool match = length > 0 && static_cast<size_t>(length) == team.size() &&
                 memcmp(utf8, team.data(), team.size()) == 0;
    OPENSSL_free(utf8);
    return match;
}

bool D2RProfileCertificate(const jvalue &profile, X509 *selected) {
    const jvalue &certificates = profile["DeveloperCertificates"];
    if (!selected || !certificates.is_array() || certificates.size() == 0 ||
        certificates.size() > 64)
        return false;
    int selectedLength = i2d_X509(selected, nullptr);
    if (selectedLength <= 0 || selectedLength > 128 * 1024)
        return false;
    string selectedDER(static_cast<size_t>(selectedLength), '\0');
    unsigned char *selectedPointer = reinterpret_cast<unsigned char *>(&selectedDER[0]);
    if (i2d_X509(selected, &selectedPointer) != selectedLength)
        return false;
    bool found = false;
    for (size_t i = 0; i < certificates.size(); ++i) {
        if (!certificates[i].is_data())
            return false;
        string der = certificates[i].as_data();
        if (der.empty() || der.size() > 128 * 1024)
            return false;
        const unsigned char *pointer = reinterpret_cast<const unsigned char *>(der.data());
        const unsigned char *end = pointer + der.size();
        X509 *candidate = d2i_X509(nullptr, &pointer, static_cast<long>(der.size()));
        if (!candidate || pointer != end) {
            X509_free(candidate);
            return false;
        }
        found = found || der == selectedDER;
        X509_free(candidate);
    }
    return found;
}

// 限制遍历深度和节点数；不猜测其他能力中的通配符含义。
// Bound traversal depth and node count; never guess wildcard meanings for other capabilities.
bool D2RConcreteEntitlements(const jvalue &value, size_t depth, size_t &remaining) {
    if (depth > 16 || remaining == 0)
        return false;
    --remaining;
    if (value.is_string())
        // 上游写入器会将这些确切形式转换为 data/date；不能悄悄改变授权值的类型。
        // The upstream writer coerces these exact forms to data/date; never silently change grant
        // types.
        return !value.is_data_string() && !value.is_date_string() &&
               value.as_string().find('*') == string::npos;
    if (value.is_array()) {
        if (value.size() > remaining)
            return false;
        for (size_t i = 0; i < value.size(); ++i)
            if (!D2RConcreteEntitlements(value[i], depth + 1, remaining))
                return false;
    } else if (value.is_object()) {
        if (value.size() > remaining)
            return false;
        vector<string> keys;
        if (!value.get_keys(keys))
            return false;
        for (const string &key : keys)
            if (key.find('*') != string::npos ||
                !D2RConcreteEntitlements(value[key], depth + 1, remaining))
                return false;
    }
    return true;
}

bool D2RProfileEntitlements(const jvalue &profile, X509 *cert, const string &bundle,
                            jvalue &entitlements, string &application, string &team,
                            bool *applicationIdentifierTooLong = nullptr) {
    if (applicationIdentifierTooLong)
        *applicationIdentifierTooLong = false;
    if (!D2RIdentifier(bundle, true))
        return false;
    const jvalue &teams = profile["TeamIdentifier"];
    const jvalue &prefixes = profile["ApplicationIdentifierPrefix"];
    if (!teams.is_array() || teams.size() != 1 || !teams[0].is_string() || !prefixes.is_array() ||
        prefixes.size() == 0 || prefixes.size() > 16 || !profile["Entitlements"].is_object())
        return false;
    team = teams[0].as_string();
    if (!D2RCertificateTeam(cert, team))
        return false;
    entitlements = profile["Entitlements"];
    if (entitlements.has("com.apple.developer.team-identifier") &&
        (!entitlements["com.apple.developer.team-identifier"].is_string() ||
         entitlements["com.apple.developer.team-identifier"].as_string() != team))
        return false;
    if (!entitlements["application-identifier"].is_string())
        return false;
    string pattern = entitlements["application-identifier"].as_string();
    size_t separator = pattern.find('.');
    if (separator == string::npos)
        return false;
    string prefix = pattern.substr(0, separator);
    size_t matching = 0;
    for (size_t i = 0; i < prefixes.size(); ++i) {
        if (!prefixes[i].is_string() || !D2RIdentifier(prefixes[i].as_string(), false))
            return false;
        if (prefixes[i].as_string() == prefix)
            ++matching;
    }
    // 旧版 App ID 前缀可以不同于团队 ID；只匹配 profile 声明的前缀。
    // A legacy App ID prefix may differ from team ID; use the profile's declared prefix.
    if (matching != 1)
        return false;
    application = prefix + "." + bundle;
    // 保留现有签名长度上限；这不是证书或描述文件不匹配。
    // Preserve the signing length limit; exceeding it is not a certificate/profile mismatch.
    if (application.size() > 255) {
        if (applicationIdentifierTooLong)
            *applicationIdentifierTooLong = true;
        return false;
    }
    if (!D2RMatchApplication(pattern, application))
        return false;
    entitlements["application-identifier"] = application;
    if (entitlements.has("keychain-access-groups")) {
        const jvalue groups = entitlements["keychain-access-groups"];
        if (!groups.is_array() || groups.size() > 64)
            return false;
        jvalue expanded(jvalue::E_ARRAY);
        set<string> seen;
        for (size_t i = 0; i < groups.size(); ++i) {
            if (!groups[i].is_string())
                return false;
            string group = groups[i].as_string();
            if (group.find('*') != string::npos) {
                if (!D2RMatchApplication(group, application))
                    return false;
                group = application;
            } else if (!D2RIdentifier(group, true))
                return false;
            // 保留 profile 明确授权的具体共享组，不凭空发明能力。
            // Preserve concrete shared groups explicitly granted by the profile; invent no
            // capabilities.
            if (seen.insert(group).second)
                expanded.push_back(group);
        }
        entitlements["keychain-access-groups"] = expanded;
    }
    size_t remaining = 4096;
    return D2RConcreteEntitlements(entitlements, 0, remaining);
}
} // namespace

bool ZSignAsset::ValidateProfileForBundle(const string &bundleId,
                                          bool *applicationIdentifierTooLong) {
    if (applicationIdentifierTooLong)
        *applicationIdentifierTooLong = false;
    if (m_bAdhoc || !m_x509Cert || m_strProvData.empty() ||
        m_strProvData.size() > D2R_PROFILE_BYTES)
        return false;
    string content;
    if (!GetCMSContent(m_strProvData, content) || content.empty() ||
        content.size() > D2R_PROFILE_BYTES || content.find("<!--") != string::npos ||
        content.find("<![CDATA[") != string::npos)
        return false;
    jvalue profile;
    bool binary = false;
    if (!profile.read_plist(content, nullptr, &binary) || binary || !profile.is_object())
        return false;
    X509 *certificate = static_cast<X509 *>(m_x509Cert);
    if (!D2RProfileCertificate(profile, certificate) ||
        !D2RProfileDates(content, certificate, time(nullptr)))
        return false;
    jvalue entitlements;
    string application, team;
    if (!D2RProfileEntitlements(profile, certificate, bundleId, entitlements, application, team,
                                applicationIdentifierTooLong))
        return false;
    string encoded;
    entitlements.style_write_plist(encoded);
    if (encoded.empty() || encoded.size() > D2R_PROFILE_BYTES)
        return false;
    m_strEntitleData = encoded;
    m_strApplicationId = application;
    m_strTeamId = team;
    return true;
}
