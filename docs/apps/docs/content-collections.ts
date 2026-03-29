import { defineCollection, defineConfig } from '@content-collections/core'
import { z } from 'zod'
import { compileMDX } from '@content-collections/mdx'
import { remarkCustomMermaid } from './lib/remark-mermaid'

const docs = defineCollection({
  name: 'docs',
  directory: 'content/docs',
  include: '**/*.mdx',
  schema: z.object({
    title: z.string(),
    description: z.string().optional(),
    category: z.string().optional(),
    order: z.number().optional(),
    project: z.string().optional(),
    projectLabel: z.string().optional(),
    projectDescription: z.string().optional(),
    projectOrder: z.number().optional(),
  }),
  transform: async (doc, context) => {
    const code = await compileMDX(context, doc, {
      remarkPlugins: [remarkCustomMermaid],
    })

    return {
      ...doc,
      code,
      slug: doc._meta.path, // Use the path as slug (e.g., 'en/introduction', 'zh/components')
    }
  },
})

export default defineConfig({
  content: [docs],
})
