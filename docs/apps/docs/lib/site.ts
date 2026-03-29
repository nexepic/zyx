export interface NavEntry {
  label: string
  labelZh?: string
  href: string
  external?: boolean
  /**
   * Slug prefixes (without locale segment), used for active state and
   * contextual sidebar filtering. Example: "getting-started".
   */
  matchPrefixes?: string[]
}

export const siteConfig = {
  name: 'ZYX',
  title: 'ZYX Documentation',
  description:
    'High-performance embedded graph database engine for RAG, agent memory, and knowledge graph applications.',
  keywords: [
    'graph database',
    'embedded database',
    'cypher',
    'vector database',
    'rag',
    'agent memory',
    'knowledge graph',
    'ai native',
    'cpp database',
  ],
  defaultLocale: 'en',
  github: process.env.NEXT_PUBLIC_GITHUB_URL || 'https://github.com/nexepic/zyx',
  docsEditEnabled: process.env.NEXT_PUBLIC_DOCS_EDIT_ENABLED === 'true',
  docsEditBase:
    process.env.NEXT_PUBLIC_DOCS_EDIT_BASE ||
    'https://github.com/nexepic/zyx/edit/main/docs',
  /**
   * Top-level navigation entries displayed in the header center.
   * Use directory-based slug prefixes to control active state + sidebar scope.
   */
  nav: [
    {
      label: 'User Guide',
      labelZh: '用户指南',
      href: '/docs/zyx/user-guide/quick-start',
      matchPrefixes: ['zyx/user-guide'],
    },
    {
      label: 'API Reference',
      labelZh: 'API 参考',
      href: '/docs/zyx/api/cpp-api',
      matchPrefixes: ['zyx/api'],
    },
    {
      label: 'Algorithms',
      labelZh: '算法',
      href: '/docs/zyx/algorithms/overview',
      matchPrefixes: ['zyx/algorithms'],
    },
    {
      label: 'Architecture',
      labelZh: '架构',
      href: '/docs/zyx/architecture/overview',
      matchPrefixes: ['zyx/architecture'],
    },
    {
      label: 'Contributing',
      labelZh: '贡献',
      href: '/docs/zyx/contributing/development-setup',
      matchPrefixes: ['zyx/contributing'],
    },
  ] as NavEntry[],
}

export function getSiteUrl(): string {
  const raw = process.env.NEXT_PUBLIC_SITE_URL || 'https://nexepic.github.io/zyx'

  try {
    const url = new URL(raw)
    return url.origin
  } catch {
    return 'https://nexepic.github.io/zyx'
  }
}

export function withSiteUrl(pathname: string): string {
  const normalizedPath = pathname.startsWith('/') ? pathname : `/${pathname}`
  return `${getSiteUrl()}${normalizedPath}`
}
