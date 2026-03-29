import { visit } from 'unist-util-visit'
import type { Plugin } from 'unified'

export const remarkCustomMermaid: Plugin = () => {
  return (tree) => {
    visit(tree, 'code', (node: any) => {
      if (node.lang === 'mermaid') {
        // Convert mermaid code blocks to Mermaid component
        node.type = 'mdxJsxFlowElement'
        node.name = 'Mermaid'
        node.attributes = [
          {
            type: 'mdxJsxAttribute',
            name: 'chart',
            value: node.value,
          },
        ]
        delete node.lang
        delete node.value
      }
    })
  }
}
