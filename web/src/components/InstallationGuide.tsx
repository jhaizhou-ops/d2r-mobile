// 准备说明始终可见；签名是可选操作，不代表已安装到设备。
// Preparation stays visible; optional signing never implies device installation.
import { t } from "../../i18n.mjs";

const references = [
  {
    label: "dmWebGuideCertificateLink",
    href: "https://developer.apple.com/help/account/certificates/create-a-certificate-signing-request",
  },
  {
    label: "dmWebGuideP12Link",
    href: "https://support.apple.com/guide/keychain-access/import-and-export-keychain-items-kyca35961/mac",
  },
  {
    label: "dmWebGuideOpenSslLink",
    href: "https://docs.openssl.org/3.0/man1/openssl-pkcs12/",
  },
  {
    label: "dmWebGuideProfileLink",
    href: "https://developer.apple.com/help/account/provisioning-profiles/create-an-ad-hoc-provisioning-profile/",
  },
] as const;

export function InstallationGuide() {
  return (
    <section aria-labelledby="preparation-heading" className="space-y-3 text-sm">
      <h2 id="preparation-heading" className="font-semibold">
        {t("dmWebGuideTitle")}
      </h2>
      <ol className="list-decimal space-y-4 pl-5 text-xs leading-relaxed text-muted-foreground">
        <li className="space-y-2 pl-1">
          <h3 className="font-medium text-foreground">{t("dmWebGuidePrepare")}</h3>
          <p>{t("dmWebGuidePackages")}</p>
          <p>{t("dmWebGuideExtracted")}</p>
          <p>{t("dmWebGuideFolderAccess")}</p>
          <p>{t("dmWebGuideExport")}</p>
        </li>
        <li className="space-y-2 pl-1">
          <h3 className="font-medium text-foreground">{t("dmWebGuideOptionalSigning")}</h3>
          <p>{t("dmWebGuideSigningIntro")}</p>
          <ol className="list-decimal space-y-2 pl-4">
            <li>{t("dmWebGuideIdentity")}</li>
            <li>{t("dmWebGuideNewCertificate")}</li>
            <li>{t("dmWebGuideProfile")}</li>
            <li>{t("dmWebGuideSigningInputs")}</li>
          </ol>
          <p>{t("dmWebGuidePrivateKey")}</p>
          <div className="flex flex-wrap gap-x-4 gap-y-2">
            {references.map(({ label, href }) => (
              <a
                key={href}
                href={href}
                target="_blank"
                rel="noopener noreferrer"
                className="text-primary underline underline-offset-4 focus-visible:rounded-sm focus-visible:ring-2 focus-visible:ring-ring"
              >
                {t(label)}
              </a>
            ))}
          </div>
        </li>
        <li className="space-y-2 pl-1">
          <h3 className="font-medium text-foreground">{t("dmWebGuideExpect")}</h3>
          <p>{t("dmWebGuideOutputs")}</p>
        </li>
      </ol>
    </section>
  );
}
