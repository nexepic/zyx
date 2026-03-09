# VitePress Documentation System Design

**Date:** 2025-03-05
**Status:** Approved
**Author:** Claude

## Overview

Implement a bilingual VitePress documentation system for the Metrix graph database project with English and Chinese language support, deployable to GitHub Pages.

## Requirements

- **Primary Language:** English
- **Secondary Language:** Chinese (Simplified)
- **URL Structure:** `/en/` for English, `/zh/` for Chinese
- **Language Switching:** Manual toggle via navigation dropdown
- **Deployment:** GitHub Pages, triggered on push to `main` branch
- **Theme:** VitePress default theme with custom branding
- **Organization:** By topic (User Guide, API, Architecture, Contributing)

## Architecture

### Technology Stack

- **VitePress 1.x:** Static site generator
- **Vue 3:** Underlying framework (VitePress dependency)
- **TypeScript:** Configuration language
- **pnpm:** Package manager for faster installs

### Directory Structure

```
docs/
├── .vitepress/
│   ├── config/
│   │   ├── index.ts          # Main configuration
│   │   ├── en.ts             # English-specific config
│   │   └── zh.ts             # Chinese-specific config
│   ├── theme/
│   │   ├── index.ts          # Custom theme overrides
│   │   └── components/       # Custom Vue components
│   └── utils/                # Helper functions
├── en/                        # English content (primary)
│   ├── index.md              # Homepage
│   ├── user-guide/
│   │   ├── installation.md
│   │   ├── quick-start.md
│   │   └── basic-operations.md
│   ├── api/
│   │   ├── cpp-api.md
│   │   ├── c-api.md
│   │   └── types.md
│   ├── architecture/
│   │   ├── overview.md
│   │   ├── storage.md
│   │   ├── query-engine.md
│   │   └── transactions.md
│   └── contributing/
│       ├── development-setup.md
│       ├── testing.md
│       └── code-style.md
├── zh/                        # Chinese content (mirrors en/)
│   └── (same structure as en/)
├── public/                    # Static assets (images, logos)
│   └── logo.png
├── package.json
└── pnpm-lock.yaml
```

### VitePress Configuration

**Main Config (`index.ts`):**
```typescript
import { defineConfig } from 'vitepress'
import en from './en'
import zh from './zh'

export default defineConfig({
  // Base URL for GitHub Pages
  base: '/metrix/',

  // Title and description
  title: 'Metrix',
  description: 'High-performance graph database engine',

  // Locale configuration
  locales: {
    root: {
      label: 'English',
      lang: 'en-US',
      themeConfig: en
    },
    zh: {
      label: '简体中文',
      lang: 'zh-CN',
      themeConfig: zh
    }
  },

  // Markdown configuration
  markdown: {
    lineNumbers: true,
    config: (md) => {
      // Add markdown-it plugins if needed
    }
  },

  // Theme configuration
  themeConfig: {
    outline: {
      level: [2, 3]
    }
  }
})
```

**English Config (`en.ts`):**
```typescript
export default {
  nav: [
    { text: 'User Guide', link: '/en/user-guide/installation' },
    { text: 'API Reference', link: '/en/api/cpp-api' },
    { text: 'Architecture', link: '/en/architecture/overview' },
    { text: 'Contributing', link: '/en/contributing/development-setup' },
    {
      text: 'Language',
      items: [
        { text: 'English', link: '/en/' },
        { text: '简体中文', link: '/zh/' }
      ]
    }
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
```

**Chinese Config (`zh.ts`):**
```typescript
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
      // ... similar structure for other sections
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
```

### GitHub Actions Workflow

**File:** `.github/workflows/docs.yml`

```yaml
name: Deploy Documentation

on:
  push:
    branches:
      - main
    paths:
      - 'docs/**'
      - '.github/workflows/docs.yml'
  workflow_dispatch:

permissions:
  contents: read
  pages: write
  id-token: write

concurrency:
  group: "pages"
  cancel-in-progress: false

jobs:
  build:
    runs-on: ubuntu-latest
    steps:
      - name: Checkout
        uses: actions/checkout@v4

      - name: Setup pnpm
        uses: pnpm/action-setup@v4
        with:
          version: 9

      - name: Setup Node.js
        uses: actions/setup-node@v4
        with:
          node-version: '20'
          cache: 'pnpm'
          cache-dependency-path: 'docs/pnpm-lock.yaml'

      - name: Install dependencies
        working-directory: ./docs
        run: pnpm install

      - name: Build VitePress site
        working-directory: ./docs
        run: pnpm run build

      - name: Upload artifact
        uses: actions/upload-pages-artifact@v3
        with:
          path: ./docs/.vitepress/dist

  deploy:
    environment:
      name: github-pages
      url: ${{ steps.deployment.outputs.page_url }}
    runs-on: ubuntu-latest
    needs: build
    steps:
      - name: Deploy to GitHub Pages
        id: deployment
        uses: actions/deploy-pages@v4
```

### Package Configuration

**`docs/package.json`:**
```json
{
  "name": "metrix-docs",
  "version": "1.0.0",
  "private": true,
  "type": "module",
  "scripts": {
    "dev": "vitepress dev",
    "build": "vitepress build",
    "preview": "vitepress preview"
  },
  "devDependencies": {
    "vitepress": "^1.6.0",
    "vue": "^3.5.13"
  }
}
```

## Development Workflow

### Git Worktree Setup

1. Create worktree for documentation development:
   ```bash
   git worktree add .worktrees/docs-feature -b feature/vitepress-docs
   ```

2. Navigate to worktree:
   ```bash
   cd .worktrees/docs-feature
   ```

3. Install dependencies and start dev server:
   ```bash
   cd docs
   pnpm install
   pnpm run dev
   ```

### Local Development

- Dev server: `http://localhost:5173`
- Access English: `http://localhost:5173/en/`
- Access Chinese: `http://localhost:5173/zh/`
- Hot reload enabled for content changes

### Content Guidelines

1. **Markdown Format:** Use GitHub Flavored Markdown
2. **Code Blocks:** Specify language for syntax highlighting
3. **Images:** Place in `docs/public/images/`, reference as `/images/filename.png`
4. **Links:** Use relative links for internal navigation
5. **Translations:** Maintain parallel structure in `/en/` and `/zh/` directories

### Deployment Process

1. Commit changes to `feature/vitepress-docs` branch
2. Create PR to `main`
3. After merge, GitHub Actions automatically builds and deploys
4. Documentation available at: `https://yuhong.github.io/metrix/`

## Success Criteria

- [ ] VitePress successfully builds without errors
- [ ] Both `/en/` and `/zh/` routes accessible
- [ ] Language toggle works in navigation
- [ ] GitHub Actions workflow deploys to GitHub Pages
- [ ] Documentation accessible at `https://yuhong.github.io/metrix/`
- [ ] All links work correctly in both languages
- [ ] Site is responsive on mobile devices

## Future Enhancements

- Add search functionality (VitePress built-in Algolia/DocSearch)
- Add diagram support (Mermaid.js for architecture diagrams)
- Add API documentation auto-generation from code comments
- Add versioned documentation for multiple releases
