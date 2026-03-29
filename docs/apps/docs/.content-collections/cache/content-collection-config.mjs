// content-collections.ts
import { defineCollection, defineConfig } from "@content-collections/core";
import { z } from "zod";
import { compileMDX } from "@content-collections/mdx";

// lib/remark-mermaid.ts
import { visit } from "unist-util-visit";
var remarkCustomMermaid = () => {
  return (tree) => {
    visit(tree, "code", (node) => {
      if (node.lang === "mermaid") {
        node.type = "mdxJsxFlowElement";
        node.name = "Mermaid";
        node.attributes = [
          {
            type: "mdxJsxAttribute",
            name: "chart",
            value: node.value
          }
        ];
        delete node.lang;
        delete node.value;
      }
    });
  };
};

// content-collections.ts
var docs = defineCollection({
  name: "docs",
  directory: "content/docs",
  include: "**/*.mdx",
  schema: z.object({
    title: z.string(),
    description: z.string().optional(),
    category: z.string().optional(),
    order: z.number().optional(),
    project: z.string().optional(),
    projectLabel: z.string().optional(),
    projectDescription: z.string().optional(),
    projectOrder: z.number().optional()
  }),
  transform: async (doc, context) => {
    const code = await compileMDX(context, doc, {
      remarkPlugins: [remarkCustomMermaid]
    });
    return {
      ...doc,
      code,
      slug: doc._meta.path
      // Use the path as slug (e.g., 'en/introduction', 'zh/components')
    };
  }
});
var content_collections_default = defineConfig({
  content: [docs]
});
export {
  content_collections_default as default
};
