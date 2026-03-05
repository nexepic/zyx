export default {
  nav: [
    { text: 'User Guide', link: '/en/user-guide/installation' },
    { text: 'API Reference', link: '/en/api/cpp-api' },
    { text: 'Architecture', link: '/en/architecture/overview' },
    { text: 'Contributing', link: '/en/contributing/development-setup' }
  ],

  sidebar: {
    '/en/': [
      {
        text: 'User Guide',
        items: [
          { text: 'Installation', link: '/en/user-guide/installation' },
          { text: 'Quick Start', link: '/en/user-guide/quick-start' },
          { text: 'Basic Operations', link: '/en/user-guide/basic-operations' }
        ]
      },
      {
        text: 'API Reference',
        items: [
          { text: 'C++ API', link: '/en/api/cpp-api' },
          { text: 'C API', link: '/en/api/c-api' },
          { text: 'Types', link: '/en/api/types' }
        ]
      },
      {
        text: 'Architecture',
        items: [
          { text: 'Overview', link: '/en/architecture/overview' },
          { text: 'Storage System', link: '/en/architecture/storage' },
          { text: 'Query Engine', link: '/en/architecture/query-engine' },
          { text: 'Transactions', link: '/en/architecture/transactions' }
        ]
      },
      {
        text: 'Contributing',
        items: [
          { text: 'Development Setup', link: '/en/contributing/development-setup' },
          { text: 'Testing', link: '/en/contributing/testing' },
          { text: 'Code Style', link: '/en/contributing/code-style' }
        ]
      }
    ]
  },

  socialLinks: [
    { icon: 'github', link: 'https://github.com/yuhong/metrix' }
  ],

  footer: {
    message: 'Released under the MIT License.',
    copyright: 'Copyright © 2025-present Metrix Contributors'
  }
}
