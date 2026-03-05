export default {
  nav: [
    { text: '用户指南', link: '/zh/user-guide/installation' },
    { text: 'API 参考', link: '/zh/api/cpp-api' },
    { text: '架构设计', link: '/zh/architecture/overview' },
    { text: '贡献指南', link: '/zh/contributing/development-setup' },
    {
      text: '语言',
      items: [
        { text: 'English', link: '/en/' },
        { text: '简体中文', link: '/zh/' }
      ]
    }
  ],

  sidebar: {
    '/zh/': [
      {
        text: '用户指南',
        items: [
          { text: '安装', link: '/zh/user-guide/installation' },
          { text: '快速开始', link: '/zh/user-guide/quick-start' },
          { text: '基本操作', link: '/zh/user-guide/basic-operations' }
        ]
      },
      {
        text: 'API 参考',
        items: [
          { text: 'C++ API', link: '/zh/api/cpp-api' },
          { text: 'C API', link: '/zh/api/c-api' },
          { text: '类型定义', link: '/zh/api/types' }
        ]
      },
      {
        text: '架构设计',
        items: [
          { text: '概述', link: '/zh/architecture/overview' },
          { text: '存储系统', link: '/zh/architecture/storage' },
          { text: '查询引擎', link: '/zh/architecture/query-engine' },
          { text: '事务管理', link: '/zh/architecture/transactions' }
        ]
      },
      {
        text: '贡献指南',
        items: [
          { text: '开发环境', link: '/zh/contributing/development-setup' },
          { text: '测试指南', link: '/zh/contributing/testing' },
          { text: '代码规范', link: '/zh/contributing/code-style' }
        ]
      }
    ]
  },

  socialLinks: [
    { icon: 'github', link: 'https://github.com/yuhong/metrix' }
  ],

  footer: {
    message: '基于 MIT 许可证发布',
    copyright: 'Copyright © 2025-present Metrix Contributors'
  }
}
