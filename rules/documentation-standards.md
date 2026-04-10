# Documentation Standards

## Visual Diagrams

1. **NO ASCII ART**: Absolutely NO hand-drawn ASCII art diagrams using box-drawing characters (┌│└─│/\ etc.)
2. **Use Mermaid Diagrams**: All diagrams MUST use Mermaid syntax
   - Supported types: `classDiagram`, `flowchart TD/LR`, `graph TD/LR`, `stateDiagram-v2`, `sequenceDiagram`, `erDiagram`
3. **Color Restrictions**: Mermaid diagrams MUST use ONLY black, white, and gray colors
   - FORBIDDEN: `fill:#90EE90`, `fill:#e1f5ff`, `fill:#ff0`, `fill:#f9f`, `fill:lightblue`, etc.
   - ALLOWED: No color styling (default, preferred), grays only (`fill:#f0f0f0`, `fill:#e0e0e0`, `fill:#d0d0d0`)
4. **Diagram Clarity**: Simple, professional, publication-ready, consistent style

## Code Examples

1. **Real Code Only**: All code examples MUST come from actual implementation, no hypothetical or pseudo-code
2. **Code Formatting**: Use proper Markdown code blocks with language tags
3. **Accuracy**: Verify all code examples are accurate and compile/test if possible

## Bilingual Documentation

1. **Chinese and English**: All documentation MUST exist in both languages
   - English: `docs/apps/docs/content/docs/en/...`
   - Chinese: `docs/apps/docs/content/docs/zh/...`
   - Both versions must be kept in sync
2. **Translation Quality**: Technically accurate, proper terminology, consistent across documents
3. **File Organization**: Mirror directory structure between `en/` and `zh/`, same filenames

## Content Structure

1. Use clear section hierarchy (##, ###, ####)
2. Link to related documentation liberally
3. Use Markdown tables for comparisons and structured data
4. Use bullet points for clarity and readability

## NexDoc Configuration

1. **Content Source**: NexDoc reads MDX from `docs/apps/docs/content/docs/{en,zh}/`
2. **Frontmatter**: Required: `title`. Recommended: `description`, `category`, `order`. Optional: `project`, `projectLabel`, `projectDescription`, `projectOrder`
3. **Navigation**: Generated from metadata in `docs/apps/docs/lib/docs.ts`. Keep category/order/project consistent.

## Review Checklist

Before submitting documentation changes:
- [ ] No ASCII art diagrams present
- [ ] All Mermaid diagrams use only black/white/gray colors
- [ ] Code examples are from actual implementation
- [ ] Both English and Chinese versions exist and are in sync
- [ ] NexDoc frontmatter metadata is complete and ordering is correct
- [ ] All links and cross-references work correctly
