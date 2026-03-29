import { redirect } from "next/navigation";
import { buildHomeJsonLd, generateSiteMetadata } from "@/lib/metadata";
import { defaultLocale, locales } from "@/lib/i18n";
import { getFirstDocHref, getProjectGroups } from "@/lib/docs";
import { ZyxHomePage as HomePageView } from "@/components/home/HomePage";

const homeMetaCopy = {
  en: {
    title: "Documentation",
    subtitle:
      "Everything you need to get started, integrate, and build with confidence.",
  },
  zh: {
    title: "文档",
    subtitle: "快速上手、集成和构建所需的一切。",
  },
} as const

export async function generateMetadata({
  params,
}: {
  params: Promise<{ locale: string }>;
}) {
  const { locale } = await params;
  const normalizedLocale = locales.includes(locale as (typeof locales)[number])
    ? locale
    : defaultLocale;
  const copy =
    homeMetaCopy[
      (normalizedLocale === "zh" ? "zh" : "en") as keyof typeof homeMetaCopy
    ];

  return generateSiteMetadata({
    title: copy.title,
    description: copy.subtitle,
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
    <HomePageView
      locale={locale}
      firstDocHref={firstDocHref}
      projects={projects}
      jsonLd={jsonLd}
    />
  );
}
