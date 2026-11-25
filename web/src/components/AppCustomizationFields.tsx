// 可选的本地应用外观字段，依据打包器的封闭校验契约。
// Optional local app identity fields, based on the packager's closed validation contract.
import { useRef } from "react";
import { t } from "../../i18n.mjs";
import { APP_CUSTOMIZATION_LIMITS } from "../../app-customization.mjs";
import { updateAppCustomizationDraft } from "../../app-customization-defaults.mjs";
import { Button } from "@/components/ui/button";
import { Input } from "@/components/ui/input";
import {
  Field,
  FieldSet,
  FieldLegend,
  FieldGroup,
  FieldLabel,
  FieldDescription,
  FieldError,
  FieldTitle,
} from "@/components/ui/field";

export type AppCustomizationDraft = {
  displayName?: string;
  bundleIdentifier?: string;
  version?: string;
  buildVersion?: string;
  icon?: File;
};
const fields = [
  {
    name: "displayName",
    section: "identity",
    label: "dmWebCustomName",
    maxLength: APP_CUSTOMIZATION_LIMITS.displayNameCharacters * 2,
  },
  {
    name: "bundleIdentifier",
    section: "identity",
    label: "dmWebCustomBundle",
    maxLength: APP_CUSTOMIZATION_LIMITS.bundleIdentifierCharacters,
  },
  {
    name: "version",
    section: "advanced",
    label: "dmWebCustomVersion",
    maxLength: APP_CUSTOMIZATION_LIMITS.versionCharacters,
  },
  { name: "buildVersion", section: "advanced", label: "dmWebCustomBuild", maxLength: 10 },
] as const;
export function AppCustomizationFields({
  section,
  value,
  defaults,
  disabled,
  onChange,
  error,
}: {
  section: "identity" | "advanced";
  value: AppCustomizationDraft;
  defaults: Readonly<
    Record<"displayName" | "bundleIdentifier" | "version" | "buildVersion", string>
  >;
  disabled: boolean;
  onChange: (value: AppCustomizationDraft) => void;
  error: string;
}) {
  const iconInput = useRef<HTMLInputElement>(null);
  return (
    <div className={section === "advanced" ? "space-y-3 border-t pt-4" : "space-y-3"}>
      {section === "advanced" && (
        <>
          <FieldTitle>{t("dmWebCustomizeApp")}</FieldTitle>
          <FieldDescription className="text-xs">{t("dmWebCustomHelp")}</FieldDescription>
        </>
      )}
      <FieldSet
        className="max-w-xl gap-4"
        disabled={disabled}
        aria-describedby={error ? "customization-error" : undefined}
      >
        <FieldLegend className="sr-only">{t("dmWebCustomizeApp")}</FieldLegend>
        <FieldGroup className="grid gap-3 sm:grid-cols-2">
          {fields
            .filter((field) => field.section === section)
            .map(({ name, label, maxLength }) => (
              <Field key={name} className="gap-2">
                <FieldLabel htmlFor={`custom-${name}`}>{t(label)}</FieldLabel>
                <Input
                  id={`custom-${name}`}
                  type="text"
                  value={value[name] ?? defaults[name]}
                  maxLength={maxLength}
                  autoComplete="off"
                  spellCheck={false}
                  onChange={(event) =>
                    onChange(updateAppCustomizationDraft(value, defaults, name, event.target.value))
                  }
                  onBlur={() => {
                    if (!disabled && value[name] === "")
                      onChange(updateAppCustomizationDraft(value, defaults, name, defaults[name]));
                  }}
                />
              </Field>
            ))}
        </FieldGroup>
        {section === "identity" &&
          value.bundleIdentifier &&
          value.bundleIdentifier !== defaults.bundleIdentifier && (
            <FieldDescription className="text-xs">{t("dmWebCustomBundleHelp")}</FieldDescription>
          )}
        {section === "advanced" && (
          <>
            <Field className="gap-2">
              <FieldLabel htmlFor="custom-icon">{t("dmWebCustomIcon")}</FieldLabel>
              <Input
                id="custom-icon"
                ref={iconInput}
                type="file"
                accept="image/png,.png"
                aria-describedby="custom-icon-help"
                onChange={(event) => onChange({ ...value, icon: event.target.files?.[0] })}
              />
              <FieldDescription id="custom-icon-help" className="text-xs">
                {t("dmWebCustomIconHelp")}
              </FieldDescription>
              {!value.icon && (
                <FieldDescription className="text-xs">
                  {t("dmWebCustomDefaultIcon")}
                </FieldDescription>
              )}
            </Field>
            <div className="flex flex-wrap gap-2">
              {value.icon && (
                <Button
                  type="button"
                  variant="outline"
                  size="sm"
                  onClick={() => {
                    if (disabled) return;
                    onChange({ ...value, icon: undefined });
                    if (iconInput.current) iconInput.current.value = "";
                  }}
                >
                  {t("dmWebCustomRemoveIcon")}
                </Button>
              )}
              <Button
                type="button"
                variant="outline"
                size="sm"
                onClick={() => {
                  if (disabled) return;
                  onChange({});
                  if (iconInput.current) iconInput.current.value = "";
                }}
              >
                {t("dmWebCustomReset")}
              </Button>
            </div>
          </>
        )}
      </FieldSet>

      {section === "identity" && error && (
        <FieldError id="customization-error" className="pb-3">
          {error}
        </FieldError>
      )}
    </div>
  );
}
