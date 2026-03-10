# 文档搜索和 AI 问答功能说明

本文档说明 VitePress 文档站点的搜索和 AI 问答功能。

## 📖 本地搜索功能

### 功能介绍

VitePress 1.6.4 内置了本地搜索功能，基于 **MiniSearch** 库，无需额外依赖，完全在浏览器端运行。

### 使用方法

1. **打开文档站点**: http://localhost:5173/metrix/
2. **点击搜索图标**: 在顶部导航栏右侧的搜索图标
3. **输入关键词**: 输入要搜索的内容
4. **查看结果**: 实时显示匹配的文档页面

### 搜索特性

- ✅ **全文搜索**: 搜索所有文档内容
- ✅ **实时结果**: 输入时即时更新搜索结果
- ✅ **双语支持**: 英文和中文文档均可搜索
- ✅ **快捷键**: `Cmd+K` (macOS) 或 `Ctrl+K` (Windows/Linux) 打开搜索框
- ✅ **结果高亮**: 搜索关键词在结果中高亮显示
- ✅ **快速导航**: 使用键盘方向键导航结果
- ✅ **无需外部服务**: 完全在浏览器端运行，不需要 API 密钥

### 搜索配置

搜索功能在 `docs/.vitepress/config/index.ts` 中配置：

```typescript
search: {
  provider: 'local'  // 使用本地搜索 (MiniSearch)
}
```

## 🤖 AI 问答功能

### ⚠️ 重要说明

**AI 问答功能仅在 Algolia DocSearch 模式下可用，本地搜索不支持 AI 功能。**

### VitePress 支持的两种搜索方式

#### 1. 本地搜索 (当前实现)

```typescript
search: {
  provider: 'local'  // 使用 MiniSearch
}
```

- ✅ **优点**:
  - 免费，无需注册
  - 完全在浏览器端运行
  - 不需要任何外部服务或 API 密钥
  - 部署简单，立即可用
- ❌ **缺点**:
  - **不支持 AI 问答功能**
  - 搜索质量略低于 Algolia
  - 对于大型文档站点可能较慢

#### 2. Algolia DocSearch (需要设置)

```typescript
search: {
  provider: 'algolia',
  options: {
    appId: 'YOUR_APP_ID',
    apiKey: 'YOUR_API_KEY',
    indexName: 'YOUR_INDEX_NAME',
    askAi: 'YOUR_ASSISTANT_ID'  // AI 问答功能
  }
}
```

- ✅ **优点**:
  - **支持 AI 问答功能**
  - 搜索质量高
  - 适合大型文档站点
- ❌ **缺点**:
  - 需要 Algolia 账户
  - 需要配置 API 密钥和爬虫
  - 需要维护索引

### 如何启用 AI 问答功能

如果需要 AI 问答功能，需要迁移到 Algolia DocSearch：

#### 步骤 1: 注册 Algolia 账户

1. 访问 https://www.algolia.com/
2. 注册并创建新应用
3. 获取 `appId` 和 `apiKey`

#### 步骤 2: 配置 DocSearch 爬虫

参考 [Algolia DocSearch 文档](https://docsearch.algolia.com/docs/what-is-docsearch/) 和 [VitePress Crawler 配置示例](https://vitepress.dev/reference/default-theme-search#crawler-config) 配置爬虫。

#### 步骤 3: 启用 Ask AI

1. 在 Algolia 仪表板中创建 AI 助手
2. 获取 `assistantId`
3. 在 VitePress 配置中添加 `askAi` 选项

#### 步骤 4: 更新 VitePress 配置

```typescript
// docs/.vitepress/config/index.ts
search: {
  provider: 'algolia',
  options: {
    appId: 'YOUR_APP_ID',
    apiKey: 'YOUR_SEARCH_API_KEY',
    indexName: 'metrix-docs',
    askAi: {
      assistantId: 'YOUR_ASSISTANT_ID',
      // 可选：覆盖默认索引配置
      // indexName: 'metrix-docs-markdown',
      // apiKey: 'YOUR_SEARCH_API_KEY',
      // appId: 'YOUR_APP_ID'
    }
  }
}
```

### AI 功能特性 (使用 Algolia 时)

- ✅ **上下文感知**: 基于文档内容回答问题
- ✅ **智能搜索**: 自动查找相关文档内容
- ✅ **代码示例**: 可以展示代码片段和示例
- ✅ **多语言**: 支持中英文文档问答
- ✅ **引用来源**: 显示答案来源的文档位置
- ✅ **对话式交互**: 支持多轮对话和追问

## 🔍 搜索框大小不一致问题

### 问题描述

本地搜索的搜索框样式可能与 VitePress 官方文档（使用 Algolia）的搜索框大小不一致。

### 原因

VitePress 官方文档使用 Algolia DocSearch，其 UI 样式与本地搜索（MiniSearch）不同。这是两种不同搜索服务的默认样式差异。

### 解决方案

这是正常现象，不需要修复。如果希望搜索框样式与官方文档一致，可以迁移到 Algolia DocSearch。

## ⚙️ 部署到生产环境

### 本地搜索模式 (当前配置)

**优点**:
- 无需额外配置
- 部署后立即可用
- 无需维护索引

**步骤**:
1. 构建文档站点: `npm run build`
2. 部署到静态托管服务（GitHub Pages, Vercel, Netlify 等）
3. 搜索功能自动可用

### Algolia DocSearch 模式 (如需 AI 功能)

**需要额外配置**:
1. 注册 Algolia 账户并获取 API 密钥
2. 配置 DocSearch 爬虫
3. 设置 Ask AI 助手
4. 更新 VitePress 配置文件
5. 测试搜索和 AI 功能

**参考文档**:
- [Algolia DocSearch 入门](https://docsearch.algolia.com/docs/what-is-docsearch/)
- [VitePress - Algolia 搜索](https://vitepress.dev/reference/default-theme-search#algolia-search)
- [VitePress - Algolia Ask AI](https://vitepress.dev/reference/default-theme-search#algolia-ask-ai-support)

## 🔍 故障排除

### 搜索功能不工作

**问题**: 搜索框不显示或搜索无结果

**解决方案**:
1. 确认使用的是 VitePress 1.6.4 或更高版本
2. 检查浏览器控制台是否有错误
3. 清除浏览器缓存并刷新页面
4. 确认 `docs/.vitepress/config/index.ts` 中已配置 `search.provider: 'local'`

### AI 问答按钮不显示

**问题**: 看不到 AI 问答按钮

**原因**: **AI 问答功能仅支持 Algolia DocSearch，本地搜索不支持 AI**

**解决方案**:
1. 如果不需要 AI 功能：保持当前本地搜索配置即可
2. 如果需要 AI 功能：按照上述步骤迁移到 Algolia DocSearch

## 📚 参考资源

- [VitePress 官方文档 - 搜索](https://vitepress.dev/reference/default-theme-search)
- [Algolia DocSearch 文档](https://docsearch.algolia.com/docs/what-is-docsearch/)
- [Algolia Ask AI 文档](https://docsearch.algolia.com/docs/v4/askai-markdown-indexing/)
- [VitePress GitHub 仓库](https://github.com/vuejs/vitepress)

## ✅ 当前状态

- ✅ **本地搜索**: 已启用，立即可用，基于 MiniSearch
- ❌ **AI 问答**: 不支持（本地搜索限制）
- ℹ️ **可选升级**: 如需 AI 功能，可迁移到 Algolia DocSearch
