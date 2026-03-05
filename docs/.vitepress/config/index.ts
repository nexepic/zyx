import { defineConfig } from 'vitepress'
import en from './en'
import zh from './zh'

export default defineConfig({
  // Base URL for GitHub Pages (update 'metrix' to your repo name)
  base: '/metrix/',

  title: 'Metrix',
  description: 'High-performance graph database engine',

  locales: {
    en: {
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

  markdown: {
    lineNumbers: true,
    mermaid: true
  },

  themeConfig: {
    outline: {
      level: [2, 3]
    }
  }
})
