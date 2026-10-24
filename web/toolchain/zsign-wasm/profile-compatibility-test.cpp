// 使用公开证书和合成元数据测试匹配逻辑；无私钥、签名或 Apple 信任验证。
// Test matching with a public certificate and synthetic metadata; no private keys, signing or Apple
// trust verification.
#include <string>
#include <set>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include "json.h"
using namespace std;

// 只提供包含文件所需的类形状；CMS 解码不是此单元测试的范围。
// Supply the class shape for the include; CMS decoding is outside this unit test.
class ZSignAsset {
  public:
    bool m_bAdhoc = false;
    void *m_x509Cert = nullptr;
    string m_strProvData, m_strEntitleData, m_strApplicationId, m_strTeamId;
    static bool GetCMSContent(const string &, string &) { return false; }
    bool ValidateProfileForBundle(const string &, bool *applicationIdentifierTooLong = nullptr);
};
#include "profile-compatibility.inc"

static jvalue profile() {
    jvalue value(jvalue::E_OBJECT);
    value["TeamIdentifier"].push_back("TEAM123456");
    value["ApplicationIdentifierPrefix"].push_back("LEGACY1234");
    value["Entitlements"]["application-identifier"] = "LEGACY1234.com.example.*";
    value["Entitlements"]["com.apple.developer.team-identifier"] = "TEAM123456";
    value["Entitlements"]["keychain-access-groups"].push_back("LEGACY1234.*");
    value["Entitlements"]["keychain-access-groups"].push_back("LEGACY1234.shared");
    return value;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    BIO *input = BIO_new_file(argv[1], "r");
    assert(input);
    X509 *publicCertificate = PEM_read_bio_X509(input, nullptr, nullptr, nullptr);
    BIO_free(input);
    assert(publicCertificate);
    unsigned char *der = nullptr;
    int length = i2d_X509(publicCertificate, &der);
    assert(length > 0);
    jvalue membership(jvalue::E_OBJECT);
    membership["DeveloperCertificates"].push_back(jvalue(jvalue::E_DATA));
    membership["DeveloperCertificates"][0].assign_data(der, static_cast<size_t>(length));
    OPENSSL_free(der);
    assert(D2RProfileCertificate(membership, publicCertificate));
    jvalue malformed = membership;
    malformed["DeveloperCertificates"][0].assign_data("bad", 3);
    assert(!D2RProfileCertificate(malformed, publicCertificate));
    malformed["DeveloperCertificates"] = jvalue(jvalue::E_ARRAY);
    assert(!D2RProfileCertificate(malformed, publicCertificate));
    input = BIO_new_file(argv[2], "r");
    assert(input);
    X509 *other = PEM_read_bio_X509(input, nullptr, nullptr, nullptr);
    BIO_free(input);
    assert(other);
    assert(!D2RProfileCertificate(membership, other));
    X509_free(other);
    X509_free(publicCertificate);

    // 元数据证书没有公钥或签名，不能用于真实签名。
    // Metadata-only certificate has no public key or signature and cannot sign anything.
    X509 *metadata = X509_new();
    X509_NAME *name = X509_NAME_new();
    assert(metadata && name);
    assert(X509_NAME_add_entry_by_NID(name, NID_organizationalUnitName, MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char *>("TEAM123456"), -1, -1,
                                      0) == 1);
    assert(X509_set_subject_name(metadata, name) == 1);
    assert(ASN1_TIME_set_string_X509(X509_getm_notBefore(metadata), "20200101000000Z") == 1);
    assert(ASN1_TIME_set_string_X509(X509_getm_notAfter(metadata), "20300101000000Z") == 1);
    assert(D2RCertificateTeam(metadata, "TEAM123456"));
    assert(!D2RCertificateTeam(metadata, "OTHER12345"));

    jvalue base = profile(), expanded;
    string application, team;
    assert(D2RProfileEntitlements(base, metadata, "com.example.app", expanded, application, team));
    assert(application == "LEGACY1234.com.example.app" && team == "TEAM123456");
    assert(expanded["keychain-access-groups"][0].as_string() == application);
    assert(expanded["keychain-access-groups"][1].as_string() == "LEGACY1234.shared");
    assert(!D2RProfileEntitlements(base, metadata, "com.other.app", expanded, application, team));
    auto reject = [&](const jvalue &invalid) {
        assert(!D2RProfileEntitlements(invalid, metadata, "com.example.app", expanded, application,
                                       team));
    };
    for (const char *pattern : {"LEGACY1234.com.*.app", "LEGACY1234.com.example*", "LEGACY1234.**",
                                "OTHER.com.example.app"}) {
        jvalue invalid = base;
        invalid["Entitlements"]["application-identifier"] = pattern;
        reject(invalid);
    }
    jvalue exact = base;
    exact["Entitlements"]["application-identifier"] = "LEGACY1234.com.example.app";
    assert(D2RProfileEntitlements(exact, metadata, "com.example.app", expanded, application, team));
    jvalue invalid = base;
    invalid["Entitlements"]["com.apple.developer.team-identifier"] = "OTHER12345";
    reject(invalid);
    invalid = base;
    invalid["TeamIdentifier"][0] = "OTHER12345";
    reject(invalid);
    invalid = base;
    invalid["ApplicationIdentifierPrefix"][0] = "TEAM123456";
    reject(invalid);
    invalid = base;
    invalid["ApplicationIdentifierPrefix"].push_back("LEGACY1234");
    reject(invalid);
    invalid = base;
    invalid["Entitlements"]["keychain-access-groups"][0] = "OTHER.*";
    reject(invalid);
    invalid = base;
    invalid["Entitlements"]["keychain-access-groups"][0] = "LEGACY1234.com*";
    reject(invalid);
    // 未解析的 iCloud 授权属于当前不支持的配置，不推断应用所需的能力。
    // Unresolved iCloud grants are unsupported; do not infer the app's capabilities.
    invalid = base;
    invalid["Entitlements"]["com.apple.developer.ubiquity-kvstore-identifier"] = "LEGACY1234.*";
    reject(invalid);
    invalid = base;
    invalid["Entitlements"]["com.apple.developer.icloud-services"].push_back("*");
    reject(invalid);
    // 上游写入器会将特殊前缀字符串转换为其他类型；门禁不得接受这种静默变化。
    // The upstream writer coerces sentinel strings to other types; reject silent
    // type changes.
    for (const char *text : {"data:YWJj", "data:", "date:2025-01-01T00:00:00Z"}) {
        invalid = base;
        invalid["Entitlements"]["com.example.metadata"].push_back(text);
        reject(invalid);
    }
    // 保留普通字符串与显式数据、布尔和整数值，不修改 CodeResources 的写入约定。
    // Retain ordinary strings and explicit data, bool and integer values without
    // changing CodeResources.
    jvalue typed = base;
    typed["Entitlements"]["com.example.text"] = "date:not-a-date";
    typed["Entitlements"]["com.example.data"].assign_data("abc", 3);
    typed["Entitlements"]["com.example.date"].assign_date(1735689600);
    typed["Entitlements"]["com.example.bool"] = true;
    typed["Entitlements"]["com.example.int"] = 7;
    assert(D2RProfileEntitlements(typed, metadata, "com.example.app", expanded, application, team));
    string serialized;
    expanded.style_write_plist(serialized);
    jvalue reparsed;
    assert(reparsed.read_plist(serialized));
    assert(reparsed["com.example.text"].is_string());
    assert(reparsed["com.example.data"].is_data());
    assert(reparsed["com.example.data"].as_data() == "abc");
    assert(reparsed["com.example.date"].is_date());
    assert(reparsed["com.example.bool"].is_bool());
    assert(reparsed["com.example.int"].is_int());
    size_t remaining = 0;
    assert(!D2RConcreteEntitlements(expanded, 0, remaining));
    remaining = 4096;
    assert(!D2RConcreteEntitlements(expanded, 17, remaining));
    assert(!D2RMatchApplication("LEGACY1234.com.*", "LEGACY1234.com."));
    assert(!D2RMatchApplication("LEGACY1234.com.*", "LEGACY1234.company.app"));

    // 长度错误只区分既有上限，不扩大匹配范围，也不限制未签名自定义。
    // Distinguish the existing length cap without expanding matches or limiting unsigned edits.
    for (const string &prefix : {string("LEGACY1234"), string(64, 'A')}) {
        jvalue boundary = base;
        boundary["ApplicationIdentifierPrefix"] = jvalue(jvalue::E_ARRAY);
        boundary["ApplicationIdentifierPrefix"].push_back(prefix);
        boundary["Entitlements"]["application-identifier"] = prefix + ".*";
        boundary["Entitlements"]["keychain-access-groups"] = jvalue(jvalue::E_ARRAY);
        const size_t supported = 255 - prefix.size() - 1;
        for (size_t length : {supported, supported + 1, size_t(255)}) {
            const string bundle = "com." + string(length - 4, 'a');
            bool tooLong = true;
            const bool accepted = D2RProfileEntitlements(boundary, metadata, bundle, expanded,
                                                         application, team, &tooLong);
            assert(accepted == (length == supported));
            assert(tooLong == (length > supported));
            assert(application.size() == prefix.size() + 1 + length);
            assert(D2RIdentifier(bundle, true));
            assert(D2RMatchApplication(prefix + ".*", application) == accepted);
            boundary["Entitlements"]["application-identifier"] = prefix + "." + bundle;
            assert(D2RProfileEntitlements(boundary, metadata, bundle, expanded, application, team,
                                          &tooLong) == accepted);
            assert(tooLong == (length > supported));
            boundary["Entitlements"]["application-identifier"] = prefix + ".*";
        }
    }
    bool tooLong = true;
    assert(!D2RProfileEntitlements(base, metadata, "com.other.app", expanded, application, team,
                                   &tooLong));
    assert(!tooLong);
    tooLong = true;
    assert(!D2RProfileEntitlements(base, metadata, "invalid..bundle", expanded, application, team,
                                   &tooLong));
    assert(!tooLong);

    const string dates = "<key>CreationDate</key><date>2020-01-01T00:00:00Z</date>"
                         "<key>ExpirationDate</key><date>2030-01-01T00:00:00Z</date>";
    assert(D2RProfileDates(dates, metadata, 1735689600)); // 2025-01-01 UTC
    assert(!D2RProfileDates(dates, metadata, 1577836799));
    assert(!D2RProfileDates(dates, metadata, 1893456000));
    assert(!D2RProfileDates(dates + dates, metadata, 1735689600));
    assert(!D2RProfileDates("<key>CreationDate</key><date>2020-02-30T00:00:00Z</date>" +
                                dates.substr(dates.find("<key>ExpirationDate")),
                            metadata, 1735689600));
    string offset = dates;
    offset.replace(offset.find("00Z"), 3, "00+01:00");
    assert(!D2RProfileDates(offset, metadata, 1735689600));
    assert(ASN1_TIME_set_string_X509(X509_getm_notAfter(metadata), "20240101000000Z") == 1);
    assert(!D2RProfileDates(dates, metadata, 1735689600));
    assert(X509_NAME_add_entry_by_NID(name, NID_organizationalUnitName, MBSTRING_ASC,
                                      reinterpret_cast<const unsigned char *>("TEAM123456"), -1, -1,
                                      0) == 1);
    assert(X509_set_subject_name(metadata, name) == 1);
    assert(!D2RCertificateTeam(metadata, "TEAM123456"));
    X509_NAME_free(name);
    X509_free(metadata);
    puts("Offline profile compatibility assertions passed (not signing or Apple trust "
         "verification).");
}
