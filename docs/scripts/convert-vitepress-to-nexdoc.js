#!/usr/bin/env node

const fs = require('fs');
const path = require('path');

// 配置
const CONFIG = {
  sourceDir: path.resolve(__dirname, '../../docs-vitepress-backup'),
  targetDir: path.resolve(__dirname, '../apps/docs/content/docs'),
  project: {
    name: 'zyx',
    label: 'ZYX',
    description: 'High-performance embedded graph database engine',
    order: 1,
  },
  categories: {
    'user-guide': 'User Guide',
    'api': 'API Reference',
    'algorithms': 'Algorithms',
    'architecture': 'Architecture',
    'contributing': 'Contributing',
  },
  // 目录到订单的映射（用于排序）
  orderMap: {
    'quick-start': 1,
    'basic-operations': 2,
    'cypher-basics': 3,
    'advanced-queries': 4,
    'pattern-matching': 5,
    'transactions': 6,
    'batch-operations': 7,
    'import-export': 8,
    'cpp-api': 1,
    'transaction': 2,
    'types': 3,
    'errors': 4,
    'c-api': 5,
    'c-types': 6,
    'c-errors': 7,
    'overview': 1,
    'segment-allocation': 2,
    'bitmap-indexing': 3,
    'compression': 4,
    'state-chain-optimistic-locking': 5,
    'wal-recovery': 6,
    'query-optimization': 7,
    'cache-eviction': 8,
    'btree-indexing': 9,
    'label-index': 10,
    'property-index': 11,
    'vector-metrics': 12,
    'product-quantization': 13,
    'diskann': 14,
    'development-setup': 1,
    'installation': 2,
    'project-structure': 3,
    'configuration': 4,
    'requirements': 5,
    'code-style': 6,
    'testing': 7,
    'writing-tests': 8,
    'doc-standards': 9,
    'vitepress-containers': 10,
  },
};

/**
 * 从 Markdown 内容中提取标题
 */
function extractTitle(content, filePath) {
  // 尝试匹配第一个 # 标题
  const h1Match = content.match(/^#\s+(.+)$/m);
  if (h1Match) {
    return h1Match[1].trim();
  }

  // 如果没有找到，使用文件名
  const fileName = path.basename(filePath, '.md');
  return fileName
    .split('-')
    .map(word => word.charAt(0).toUpperCase() + word.slice(1))
    .join(' ');
}

/**
 * 从 Markdown 内容中提取描述
 */
function extractDescription(content) {
  // 移除 frontmatter（如果有）
  let cleanContent = content.replace(/^---[\s\S]*?---\n/, '');

  // 移除标题
  cleanContent = cleanContent.replace(/^#\s+.+$/m, '');

  // 查找第一个段落
  const paragraphMatch = cleanContent.match(/^([^\n#]+)$/m);
  if (paragraphMatch) {
    let description = paragraphMatch[1].trim();
    // 清理特殊字符
    description = description
      .replace(/\*\*/g, '')
      .replace(/\*/g, '')
      .replace(/`/g, '')
      .replace(/\[([^\]]+)\]\([^)]+\)/g, '$1')
      .substring(0, 150);
    return description;
  }

  return 'Documentation for ZYX graph database engine.';
}

/**
 * 生成 frontmatter
 */
function generateFrontmatter(title, description, category, order, locale) {
  const { project } = CONFIG;

  return `---
title: ${title}
description: ${description}
category: ${category}
order: ${order}
project: ${project.name}
projectLabel: ${project.label}
projectDescription: ${project.description}
projectOrder: ${project.order}
---
`;
}

/**
 * 更新内部链接
 */
function updateInternalLinks(content, locale) {
  // 更新 VitePress 链接格式到 NexDoc 格式
  // /en/user-guide/quick-start -> /en/zyx/user-guide/quick-start

  return content.replace(
    new RegExp(`\\/(${locale})\\/([a-z\\-]+)`, 'g'),
    `/$1/zyx/$2`
  );
}

/**
 * 处理单个文件
 */
function processFile(sourcePath, targetPath, locale, category, fileName) {
  try {
    let content = fs.readFileSync(sourcePath, 'utf-8');

    // 提取标题和描述
    const title = extractTitle(content, sourcePath);
    const description = extractDescription(content);

    // 获取排序
    const order = CONFIG.orderMap[fileName] || 99;

    // 生成 frontmatter
    const frontmatter = generateFrontmatter(title, description, category, order, locale);

    // 移除现有的 frontmatter（如果有）
    let cleanContent = content.replace(/^---[\s\S]*?---\n/, '');

    // 更新内部链接
    cleanContent = updateInternalLinks(cleanContent, locale);

    // 组合最终内容
    const finalContent = frontmatter + cleanContent;

    // 确保目标目录存在
    const targetDir = path.dirname(targetPath);
    if (!fs.existsSync(targetDir)) {
      fs.mkdirSync(targetDir, { recursive: true });
    }

    // 写入文件
    fs.writeFileSync(targetPath, finalContent, 'utf-8');

    console.log(`✓ Converted: ${sourcePath} -> ${targetPath}`);
    return true;
  } catch (error) {
    console.error(`✗ Error processing ${sourcePath}:`, error.message);
    return false;
  }
}

/**
 * 主函数
 */
function main() {
  console.log('🚀 Starting VitePress to NexDoc conversion...\n');

  const { sourceDir, targetDir, categories } = CONFIG;

  // 处理英文和中文文档
  const locales = ['en', 'zh'];

  locales.forEach(locale => {
    console.log(`\n📚 Processing ${locale.toUpperCase()} documents...`);

    const localeSourceDir = path.join(sourceDir, locale);
    const localeTargetDir = path.join(targetDir, locale);

    if (!fs.existsSync(localeSourceDir)) {
      console.log(`⚠️  Source directory not found: ${localeSourceDir}`);
      return;
    }

    // 遍历所有类别目录
    Object.entries(categories).forEach(([dirName, categoryName]) => {
      const categorySourceDir = path.join(localeSourceDir, dirName);

      if (!fs.existsSync(categorySourceDir)) {
        console.log(`⚠️  Category directory not found: ${categorySourceDir}`);
        return;
      }

      console.log(`\n  📂 Processing category: ${categoryName}`);

      // 读取目录中的所有文件
      const files = fs.readdirSync(categorySourceDir);

      files.forEach(file => {
        if (!file.endsWith('.md')) {
          return;
        }

        const sourcePath = path.join(categorySourceDir, file);
        const fileName = path.basename(file, '.md');
        const targetFileName = `${fileName}.mdx`;
        const targetPath = path.join(
          localeTargetDir,
          CONFIG.project.name,
          dirName,
          targetFileName
        );

        processFile(sourcePath, targetPath, locale, categoryName, fileName);
      });
    });
  });

  console.log('\n✅ Conversion completed!\n');
  console.log(`📊 Statistics:`);
  console.log(`   Source: ${sourceDir}`);
  console.log(`   Target: ${targetDir}`);
  console.log(`   Project: ${CONFIG.project.name} (${CONFIG.project.label})`);
}

// 运行脚本
if (require.main === module) {
  main();
}

module.exports = { processFile, generateFrontmatter };
