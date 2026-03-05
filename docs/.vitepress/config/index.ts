import { defineConfig } from 'vitepress'
import en from './en'
import zh from './zh'

export default defineConfig({
  title: 'Metrix',
  description: 'High-performance graph database engine',

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

  markdown: {
    lineNumbers: true
  },

  themeConfig: {
    outline: {
      level: [2, 3]
    }
  }
})
