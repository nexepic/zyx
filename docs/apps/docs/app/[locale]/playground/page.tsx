import { redirect } from "next/navigation";
import { generateSiteMetadata } from "@/lib/metadata";
import { defaultLocale, locales } from "@/lib/i18n";
import { PlaygroundPage } from "@/home/playground-page";

export async function generateMetadata({
  params,
}: {
  params: Promise<{ locale: string }>;
}) {
  const { locale } = await params;
  const normalizedLocale = locales.includes(locale as (typeof locales)[number])
    ? locale
    : defaultLocale;
  const isEn = normalizedLocale === "en";

  return generateSiteMetadata({
    title: isEn ? "Cypher Playground" : "Cypher 试验场",
    description: isEn
      ? "Interactive Cypher query playground powered by ZYX WebAssembly engine."
      : "基于 ZYX WebAssembly 引擎的交互式 Cypher 查询试验场。",
    pathname: `/${normalizedLocale}/playground`,
    locale: normalizedLocale,
  });
}

export default async function Playground({
  params,
}: {
  params: Promise<{ locale: string }>;
}) {
  const { locale } = await params;

  if (!locales.includes(locale as (typeof locales)[number])) {
    redirect(`/${defaultLocale}/playground`);
  }

  const isEn = locale === "en-US" || locale === "en";

  return <PlaygroundPage isEn={isEn} locale={locale} />;
}
