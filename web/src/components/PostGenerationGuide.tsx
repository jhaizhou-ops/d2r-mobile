// 使用已提交结果确定输出名称及复制指引，而不是请求的选项。
// Use the committed result, not requested options, for output names and copy instructions.
import { t } from "../../i18n.mjs";
import { createPostGenerationGuide } from "../../post-generation-guide.mjs";
import { Card, CardContent, CardHeader, CardTitle } from "@/components/ui/card";

export function PostGenerationGuide({
  output,
  downloadFileName,
  signed,
}: {
  output: NonNullable<ReturnType<typeof createPostGenerationGuide>>;
  downloadFileName: string;
  signed: boolean;
}) {
  return (
    <Card className="gap-3 border-primary/20 bg-primary/5 py-4 shadow-none">
      <CardHeader className="px-4">
        <CardTitle role="heading" aria-level={2} className="text-base">
          {t("dmWebResultTitle")}
        </CardTitle>
      </CardHeader>
      <CardContent className="space-y-3 px-4 text-sm leading-relaxed">
        <ol className="list-decimal space-y-3 pl-5">
          <li className="break-words">
            {t("dmWebResultInstall", downloadFileName)}
            <p className="mt-1 text-xs text-muted-foreground">
              {t(signed ? "dmWebGuideSigned" : "dmWebResultUnsigned")}
            </p>
          </li>
          <li className="break-words">{t("dmWebResultLaunch", output.appName)}</li>
          {output.resourcesDirectory && (
            <li className="break-words">
              {t("dmWebResultCopy", output.resourcesDirectory, output.appName)}
            </li>
          )}
          {output.legacyDirectory && (
            <li className="break-words">
              {t("dmWebResultCopy", output.legacyDirectory, output.appName)}
            </li>
          )}
        </ol>
        {output.resourcesStatus !== "complete" && (
          <p
            role="note"
            className={
              output.resourcesStatus === "incomplete" ? "text-destructive" : "text-muted-foreground"
            }
          >
            {t(
              output.resourcesStatus === "skipped" ? "dmWebResultSkipped" : "dmWebResultIncomplete",
            )}
          </p>
        )}
        {output.legacyIncomplete && (
          <p role="note" className="text-destructive">
            {t("dmWebResultLegacyIncomplete")}
          </p>
        )}
        {(output.resourcesDirectory || output.legacyDirectory) && (
          <div className="space-y-2 border-t border-primary/15 pt-3 text-xs text-muted-foreground">
            <p>{t("dmWebGuideTransfer")}</p>
            <p>{t("dmWebGuideRetry")}</p>
          </div>
        )}
      </CardContent>
    </Card>
  );
}
