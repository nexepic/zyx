import { defineNexDocConfig } from '@/lib/config'
import { withMermaid } from '@/lib/plugins/mermaid'

const baseSiteConfig = defineNexDocConfig({
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
  assets: {
    brand: {
      iconLight: '/assets/branding/icon.svg',
      iconDark: '/assets/branding/icon-light.svg',
      alt: 'ZYX',
    },
    icons: {
      icon: '/assets/branding/icon.svg',
      favicon: '/assets/branding/icon.svg',
    },
  },
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
  ],
})

const config = withMermaid(baseSiteConfig, {
  enabled: true,
})

export default config
