import { withMermaid } from "vitepress-plugin-mermaid";
import en from './en'
import zh from './zh'

// Define the config without the wrapper
const config = {
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
    lineNumbers: true
  },

  themeConfig: {
    outline: {
      level: [2, 3]
    }
  },

  // Mermaid configuration
  mermaid: {
    theme: 'default'
  }
}

// Export with mermaid wrapper
export default withMermaid(config)
