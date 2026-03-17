export default {
  nav: [
    { text: '用户指南', link: '/zh/user-guide/quick-start', activeMatch: '/zh/user-guide/' },
    { text: 'API 参考', link: '/zh/api/cpp-api', activeMatch: '/zh/api/' },
    { text: '算法详解', link: '/zh/algorithms/overview', activeMatch: '/zh/algorithms/' },
    { text: '架构设计', link: '/zh/architecture/overview', activeMatch: '/zh/architecture/' },
    { text: '贡献指南', link: '/zh/contributing/development-setup', activeMatch: '/zh/contributing/' }
  ],

  sidebar: {
    '/zh/user-guide/': [
      {
        text: '入门指南',
        items: [
          { text: '快速开始', link: '/zh/user-guide/quick-start' },
          { text: '基本操作', link: '/zh/user-guide/basic-operations' }
        ]
      },
      {
        text: 'Cypher 查询语言',
        items: [
          { text: 'Cypher 基础', link: '/zh/user-guide/cypher-basics' },
          { text: '高级查询', link: '/zh/user-guide/advanced-queries' },
          { text: '模式匹配', link: '/zh/user-guide/pattern-matching' }
        ]
      },
      {
        text: 'CLI 功能',
        items: [
          { text: '事务控制', link: '/zh/user-guide/transactions' },
          { text: '批处理操作', link: '/zh/user-guide/batch-operations' },
          { text: '导入与导出', link: '/zh/user-guide/import-export' }
        ]
      }
    ],
    '/zh/api/': [
      {
        text: 'C++ API',
        items: [
          { text: '数据库类', link: '/zh/api/cpp-api' },
          { text: '事务类', link: '/zh/api/transaction' },
          { text: '值类型', link: '/zh/api/types' },
          { text: '错误处理', link: '/zh/api/errors' }
        ]
      },
      {
        text: 'C API',
        items: [
          { text: '函数', link: '/zh/api/c-api' },
          { text: '类型', link: '/zh/api/c-types' },
          { text: '错误码', link: '/zh/api/c-errors' }
        ]
      }
    ],
    '/zh/algorithms/': [
      {
        text: '算法概述',
        items: [
          { text: '概述', link: '/zh/algorithms/overview' }
        ]
      },
      {
        text: '存储算法',
        items: [
          { text: '段分配', link: '/zh/algorithms/segment-allocation' },
          { text: '位图索引', link: '/zh/algorithms/bitmap-indexing' },
          { text: '数据压缩', link: '/zh/algorithms/compression' }
        ]
      },
      {
        text: '事务算法',
        items: [
          { text: '状态链与乐观锁', link: '/zh/algorithms/state-chain-optimistic-locking' },
          { text: 'WAL 恢复', link: '/zh/algorithms/wal-recovery' }
        ]
      },
      {
        text: '查询算法',
        items: [
          { text: '查询优化', link: '/zh/algorithms/query-optimization' }
        ]
      },
      {
        text: '缓存算法',
        items: [
          { text: '缓存淘汰', link: '/zh/algorithms/cache-eviction' }
        ]
      },
      {
        text: '索引算法',
        items: [
          { text: 'B+树索引', link: '/zh/algorithms/btree-indexing' },
          { text: '标签索引', link: '/zh/algorithms/label-index' },
          { text: '属性索引', link: '/zh/algorithms/property-index' }
        ]
      },
      {
        text: '向量算法',
        items: [
          { text: 'DiskANN', link: '/zh/algorithms/diskann' },
          { text: '乘积量化', link: '/zh/algorithms/product-quantization' },
          { text: 'K-Means 聚类', link: '/zh/algorithms/kmeans' },
          { text: '向量度量', link: '/zh/algorithms/vector-metrics' }
        ]
      },
      {
        text: '遍历算法',
        items: [
          { text: '关系遍历', link: '/zh/algorithms/relationship-traversal' }
        ]
      }
    ],
    '/zh/architecture/': [
      {
        text: '核心架构',
        items: [
          { text: '概述', link: '/zh/architecture/overview' },
          { text: '存储系统', link: '/zh/architecture/storage' },
          { text: '查询引擎', link: '/zh/architecture/query-engine' },
          { text: '事务系统', link: '/zh/architecture/transactions' }
        ]
      },
      {
        text: '存储细节',
        items: [
          { text: '段格式', link: '/zh/architecture/segment-format' },
          { text: 'WAL 实现', link: '/zh/architecture/wal' },
          { text: '缓存管理', link: '/zh/architecture/cache' }
        ]
      },
      {
        text: '性能',
        items: [
          { text: '优化策略', link: '/zh/architecture/optimization' },
          { text: '基准测试', link: '/zh/architecture/benchmarks' }
        ]
      }
    ],
    '/zh/contributing/': [
      {
        text: '快速开始',
        items: [
          { text: '安装指南', link: '/zh/contributing/installation' },
          { text: '系统要求', link: '/zh/contributing/requirements' },
          { text: '系统配置', link: '/zh/contributing/configuration' },
          { text: '开发环境', link: '/zh/contributing/development-setup' }
        ]
      },
      {
        text: '开发',
        items: [
          { text: '项目结构', link: '/zh/contributing/project-structure' },
          { text: '代码规范', link: '/zh/contributing/code-style' }
        ]
      },
      {
        text: '测试',
        items: [
          { text: '测试指南', link: '/zh/contributing/testing' },
          { text: '编写测试', link: '/zh/contributing/writing-tests' }
        ]
      },
      {
        text: '文档',
        items: [
          { text: '文档标准', link: '/zh/contributing/doc-standards' },
          { text: 'VitePress 容器指南', link: '/zh/contributing/vitepress-containers' }
        ]
      }
    ]
  },

  socialLinks: [
    { icon: 'github', link: 'https://github.com/nexepic/zyx' }
  ],

  footer: {
    message: '基于 MIT 许可证发布',
    copyright: 'Copyright © 2025-present ZYX Contributors'
  }
}
