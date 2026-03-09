# 文档搜索和 AI 问答功能说明

本文档说明 VitePress 文档站点的搜索和 AI 问答功能。

## 📖 本地搜索功能

### 功能介绍

VitePress 1.6.4 内置了本地搜索功能，无需额外依赖，完全在浏览器端运行。

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

### 搜索配置

搜索功能在 `docs/.vitepress/config/index.ts` 中配置：

```typescript
search: {
  provider: 'local'  // 使用本地搜索
}
```

## 🤖 AI 问答功能

### 功能介绍

VitePress AI 提供智能文档问答功能，可以基于文档内容回答用户问题。

### 使用方法

1. **打开文档站点**: http://localhost:5173/metrix/
2. **点击 AI 图标**: 在页面右下角的 AI 聊天图标
3. **输入问题**: 用自然语言提问，例如：
   - "如何创建一个数据库？"
   - "什么是 DiskANN 算法？"
   - "如何使用事务？"
4. **获取答案**: AI 基于文档内容提供准确的答案

### AI 功能特性

- ✅ **上下文感知**: 基于当前文档页面回答问题
- ✅ **智能搜索**: 自动查找相关文档内容
- ✅ **代码示例**: 可以展示代码片段和示例
- ✅ **多语言**: 支持中英文文档问答
- ✅ **引用来源**: 显示答案来源的文档位置

### AI 配置

AI 功能在 `docs/.vitepress/config/index.ts` 中配置：

```typescript
ai: {
  pergola: {
    pergolaEndpoint: 'https://https://app.arc53.ai/v1/api'
  }
}
```

## ⚙️ 部署到生产环境

### 搜索功能

搜索功能完全内置，无需额外配置。部署文档站点后，搜索功能自动可用。

### AI 问答功能

AI 问答功能需要：

1. **API 密钥或服务集成**:
   - 申请 VitePress AI API 密钥
   - 或配置兼容的 AI 服务端点

2. **配置环境变量** (可选):
   ```bash
   export VITEPRESS_AI_API_KEY="your-api-key"
   ```

3. **验证功能**:
   - 在生产环境中测试 AI 问答
   - 确保 API 端点可访问

## 🔍 故障排除

### 搜索功能不工作

**问题**: 搜索框不显示或搜索无结果

**解决方案**:
1. 确认使用的是 VitePress 1.6.4 或更高版本
2. 检查浏览器控制台是否有错误
3. 清除浏览器缓存并刷新页面
4. 确认 `docs/.vitepress/config/index.ts` 中已配置 `search.provider: 'local'`

### AI 问答不工作

**问题**: AI 图标不显示或无法获取答案

**解决方案**:
1. AI 功能可能需要 API 密钥配置
2. 检查网络连接和 API 端点可访问性
3. 查看 VitePress AI 文档了解完整配置
4. 某些功能可能需要付费订阅

## 📚 更多信息

- [VitePress 官方文档 - 搜索](https://vitepress.dev/guide/default-theme-search.html)
- [VitePress 官方文档 - AI](https://vitepress.dev/guide/ai.html)
- [VitePress GitHub 仓库](https://github.com/vuejs/vitepress)

## ✅ 当前状态

- ✅ 本地搜索: 已启用，立即可用
- ⚠️ AI 问答: 已配置，可能需要额外设置（API 密钥等）
