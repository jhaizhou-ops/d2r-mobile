// 常见问题复用现有 Accordion；不折叠上方准备说明。
// FAQ reuses the existing Accordion without hiding the preparation instructions.
import { t } from "../../i18n.mjs";
import {
  Accordion,
  AccordionContent,
  AccordionItem,
  AccordionTrigger,
} from "@/components/ui/accordion";

const questions = [
  ["dmWebFaqSigningQuestion", "dmWebFaqSigningAnswer"],
  ["dmWebFaqInputsQuestion", "dmWebFaqInputsAnswer"],
  ["dmWebFaqIosQuestion", "dmWebGuideTargets"],
  ["dmWebFaqAssetsQuestion", "dmWebFaqAssetsAnswer"],
  ["dmWebFaqPrivacyQuestion", "dmWebFaqPrivacyAnswer"],
  ["dmWebFaqCacheQuestion", "dmWebFaqCacheAnswer"],
] as const;

export function PreparationFaq() {
  return (
    <section aria-labelledby="faq-heading" className="space-y-1">
      <h2 id="faq-heading" className="text-sm font-semibold">
        {t("dmWebFaqTitle")}
      </h2>
      <Accordion type="single" collapsible>
        {questions.map(([question, answer]) => (
          <AccordionItem key={question} value={question}>
            <AccordionTrigger className="min-h-11 py-3 text-sm">{t(question)}</AccordionTrigger>
            <AccordionContent className="text-xs leading-relaxed text-muted-foreground">
              {t(answer)}
            </AccordionContent>
          </AccordionItem>
        ))}
      </Accordion>
    </section>
  );
}
