export default {
  nav: [
    { text: 'User Guide', link: '/en/user-guide/quick-start', activeMatch: '/en/user-guide/' },
    { text: 'API Reference', link: '/en/api/cpp-api', activeMatch: '/en/api/' },
    { text: 'Algorithms', link: '/en/algorithms/overview', activeMatch: '/en/algorithms/' },
    { text: 'Architecture', link: '/en/architecture/overview', activeMatch: '/en/architecture/' },
    { text: 'Contributing', link: '/en/contributing/development-setup', activeMatch: '/en/contributing/' }
  ],

  sidebar: {
    '/en/user-guide/': [
      {
        text: 'Getting Started',
        items: [
          { text: 'Quick Start', link: '/en/user-guide/quick-start' },
          { text: 'Basic Operations', link: '/en/user-guide/basic-operations' }
        ]
      },
      {
        text: 'Cypher Query Language',
        items: [
          { text: 'Cypher Basics', link: '/en/user-guide/cypher-basics' },
          { text: 'Advanced Queries', link: '/en/user-guide/advanced-queries' },
          { text: 'Pattern Matching', link: '/en/user-guide/pattern-matching' }
        ]
      },
      {
        text: 'CLI Features',
        items: [
          { text: 'Transaction Control', link: '/en/user-guide/transactions' },
          { text: 'Batch Operations', link: '/en/user-guide/batch-operations' },
          { text: 'Import & Export', link: '/en/user-guide/import-export' }
        ]
      }
    ],
    '/en/api/': [
      {
        text: 'C++ API',
        items: [
          { text: 'Database Class', link: '/en/api/cpp-api' },
          { text: 'Transaction Class', link: '/en/api/transaction' },
          { text: 'Value Types', link: '/en/api/types' },
          { text: 'Error Handling', link: '/en/api/errors' }
        ]
      },
      {
        text: 'C API',
        items: [
          { text: 'Functions', link: '/en/api/c-api' },
          { text: 'Types', link: '/en/api/c-types' },
          { text: 'Error Codes', link: '/en/api/c-errors' }
        ]
      }
    ],
    '/en/algorithms/': [
      {
        text: 'Algorithms Overview',
        items: [
          { text: 'Overview', link: '/en/algorithms/overview' }
        ]
      },
      {
        text: 'Storage Algorithms',
        items: [
          { text: 'Segment Allocation', link: '/en/algorithms/segment-allocation' },
          { text: 'Bitmap Indexing', link: '/en/algorithms/bitmap-indexing' },
          { text: 'Compression', link: '/en/algorithms/compression' }
        ]
      },
      {
        text: 'Transaction Algorithms',
        items: [
          { text: 'State Chain & Optimistic Locking', link: '/en/algorithms/state-chain-optimistic-locking' },
          { text: 'WAL Recovery', link: '/en/algorithms/wal-recovery' }
        ]
      },
      {
        text: 'Query Algorithms',
        items: [
          { text: 'Query Optimization', link: '/en/algorithms/query-optimization' }
        ]
      },
      {
        text: 'Caching Algorithms',
        items: [
          { text: 'Cache Eviction', link: '/en/algorithms/cache-eviction' }
        ]
      }
    ],
    '/en/architecture/': [
      {
        text: 'Core Architecture',
        items: [
          { text: 'Overview', link: '/en/architecture/overview' },
          { text: 'Storage System', link: '/en/architecture/storage' },
          { text: 'Query Engine', link: '/en/architecture/query-engine' },
          { text: 'Transaction System', link: '/en/architecture/transactions' }
        ]
      },
      {
        text: 'Storage Details',
        items: [
          { text: 'Segment Format', link: '/en/architecture/segment-format' },
          { text: 'WAL Implementation', link: '/en/architecture/wal' },
          { text: 'Cache Management', link: '/en/architecture/cache' }
        ]
      },
      {
        text: 'Performance',
        items: [
          { text: 'Optimization Strategies', link: '/en/architecture/optimization' },
          { text: 'Benchmarks', link: '/en/architecture/benchmarks' }
        ]
      }
    ],
    '/en/contributing/': [
      {
        text: 'Getting Started',
        items: [
          { text: 'Installation', link: '/en/contributing/installation' },
          { text: 'System Requirements', link: '/en/contributing/requirements' },
          { text: 'Configuration', link: '/en/contributing/configuration' },
          { text: 'Development Setup', link: '/en/contributing/development-setup' }
        ]
      },
      {
        text: 'Development',
        items: [
          { text: 'Project Structure', link: '/en/contributing/project-structure' },
          { text: 'Code Style', link: '/en/contributing/code-style' }
        ]
      },
      {
        text: 'Testing',
        items: [
          { text: 'Testing Guidelines', link: '/en/contributing/testing' },
          { text: 'Writing Tests', link: '/en/contributing/writing-tests' }
        ]
      },
      {
        text: 'Documentation',
        items: [
          { text: 'Doc Standards', link: '/en/contributing/doc-standards' }
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
