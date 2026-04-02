import type { Metadata } from 'next'
import { locales } from '@/lib/i18n'
import { siteConfig, getSiteUrl, withSiteUrl } from '@/lib/site'

interface SiteMetadataInput {
  title?: string
  description?: string
  pathname: string
  locale?: string
  noIndex?: boolean
}

function localeAlternates(pathname: string): Record<string, string> {
  const alternates: Record<string, string> = {}
  const rawSegments = pathname.split('/').filter(Boolean)
  const localeIndex = rawSegments.findIndex((segment) =>
    locales.includes(segment as (typeof locales)[number])
  )

  for (const locale of locales) {
    const localizedSegments =
      localeIndex === -1
        ? [locale, ...rawSegments]
        : [
            ...rawSegments.slice(0, localeIndex),
            locale,
            ...rawSegments.slice(localeIndex + 1),
          ]
    const normalized = `/${localizedSegments.join('/')}`.replace(/\/+$/, '') || '/'

    alternates[locale] = withSiteUrl(normalized)
  }

  return alternates
}

function toAbsoluteAssetUrl(assetPath: string): string {
  if (/^https?:\/\//.test(assetPath)) {
    return assetPath
  }
  return withSiteUrl(assetPath.startsWith('/') ? assetPath : `/${assetPath}`)
}

export function generateSiteMetadata({
  title,
  description,
  pathname,
  locale,
  noIndex = false,
}: SiteMetadataInput): Metadata {
  const fullTitle = title ? `${title} | ${siteConfig.name}` : siteConfig.title
  const fullDescription = description || siteConfig.description
  const canonicalUrl = withSiteUrl(pathname)
  const favicon =
    siteConfig.assets?.icons?.favicon
    || siteConfig.assets?.icons?.icon
    || siteConfig.brand?.favicon
    || '/favicon.ico'
  const icon = siteConfig.assets?.icons?.icon || favicon
  const shortcut = siteConfig.assets?.icons?.shortcut || favicon
  const apple = siteConfig.assets?.icons?.apple || favicon
  const ogImage = siteConfig.assets?.social?.ogImage
  const twitterImage = siteConfig.assets?.social?.twitterImage || ogImage

  return {
    metadataBase: new URL(getSiteUrl()),
    applicationName: siteConfig.name,
    title: fullTitle,
    description: fullDescription,
    keywords: siteConfig.keywords,
    category: 'technology',
    alternates: {
      canonical: canonicalUrl,
      languages: localeAlternates(pathname),
    },
    icons: {
      icon: [{ url: icon }],
      shortcut: [{ url: shortcut }],
      apple: [{ url: apple }],
    },
    manifest: siteConfig.assets?.manifest,
    robots: {
      index: !noIndex,
      follow: !noIndex,
      googleBot: {
        index: !noIndex,
        follow: !noIndex,
        'max-image-preview': 'large',
        'max-snippet': -1,
        'max-video-preview': -1,
      },
    },
    openGraph: {
      type: 'website',
      locale,
      siteName: siteConfig.name,
      title: fullTitle,
      description: fullDescription,
      url: canonicalUrl,
      images: ogImage ? [toAbsoluteAssetUrl(ogImage)] : undefined,
    },
    twitter: {
      card: 'summary_large_image',
      title: fullTitle,
      description: fullDescription,
      images: twitterImage ? [toAbsoluteAssetUrl(twitterImage)] : undefined,
    },
  }
}

export function buildHomeJsonLd(locale: string): Record<string, unknown> {
  return {
    '@context': 'https://schema.org',
    '@type': 'WebSite',
    name: siteConfig.name,
    description: siteConfig.description,
    inLanguage: locale,
    url: withSiteUrl(`/${locale}`),
    potentialAction: {
      '@type': 'SearchAction',
      target: `${withSiteUrl(`/${locale}`)}?q={search_term_string}`,
      'query-input': 'required name=search_term_string',
    },
  }
}

export function buildDocJsonLd(params: {
  locale: string
  title: string
  description?: string
  pathname: string
}): Record<string, unknown> {
  return {
    '@context': 'https://schema.org',
    '@type': 'TechArticle',
    headline: params.title,
    description: params.description || siteConfig.description,
    inLanguage: params.locale,
    mainEntityOfPage: withSiteUrl(params.pathname),
    url: withSiteUrl(params.pathname),
    publisher: {
      '@type': 'Organization',
      name: siteConfig.name,
      url: getSiteUrl(),
    },
  }
}
