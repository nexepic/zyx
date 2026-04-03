import { redirect } from "next/navigation";
import { buildHomeJsonLd, generateSiteMetadata } from "@/lib/metadata";
import { defaultLocale, locales } from "@/lib/i18n";
import { getFirstDocHref, getProjectGroups } from "@/lib/docs";
import { siteConfig } from "@/lib/site";
import { CustomHomePage } from "@/home/custom-home";

const defaultHomeMetadata = {
  en: {
    title: "Documentation",
    description:
      "Everything you need to get started, integrate, and build with confidence.",
  },
  zh: {
    title: "文档",
    description: "快速上手、集成和构建所需的一切。",
  },
} as const;

export async function generateMetadata({
  params,
}: {
  params: Promise<{ locale: string }>;
}) {
  const { locale } = await params;
  const normalizedLocale = locales.includes(locale as (typeof locales)[number])
    ? locale
    : defaultLocale;
  const localeKey = (normalizedLocale === "zh" ? "zh" : "en") as keyof typeof defaultHomeMetadata;
  const defaults = defaultHomeMetadata[localeKey];
  const configuredHomeMetadata = siteConfig.home?.metadata;
  const fallbackMetadata = configuredHomeMetadata?.default;
  const localizedMetadata = configuredHomeMetadata?.[normalizedLocale];
  const title = localizedMetadata?.title || fallbackMetadata?.title || defaults.title;
  const description =
    localizedMetadata?.description || fallbackMetadata?.description || defaults.description;

  return generateSiteMetadata({
    title,
    description,
    pathname: `/${normalizedLocale}`,
    locale: normalizedLocale,
  });
}

export default async function LocaleHomePage({
  params,
}: {
  params: Promise<{ locale: string }>;
}) {
  const { locale } = await params;

  if (!locales.includes(locale as (typeof locales)[number])) {
    redirect(`/${defaultLocale}`);
  }

  const firstDocHref = getFirstDocHref(locale) || `/${locale}/docs`;
  const projects = getProjectGroups(locale);
  const jsonLd = buildHomeJsonLd(locale);

  return (
    <CustomHomePage
      locale={locale}
      firstDocHref={firstDocHref}
      projects={projects}
      jsonLd={jsonLd}
    />
  );
}
