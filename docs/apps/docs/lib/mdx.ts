const ADMONITION_TYPE_MAP: Record<string, 'info' | 'warning' | 'danger' | 'success'> = {
  info: 'info',
  note: 'info',
  tip: 'success',
  warning: 'warning',
  danger: 'danger',
  success: 'success',
}

function escapeAttrValue(value: string): string {
  return value.replace(/&/g, '&amp;').replace(/"/g, '&quot;')
}

/**
 * Doc pages already render title/description from frontmatter in the page header.
 * To avoid duplicated top headings, strip a leading markdown H1 if present.
 */
export function stripLeadingH1(source: string): string {
  const lines = source.split('\n')
  let index = 0

  while (index < lines.length && lines[index].trim() === '') {
    index += 1
  }

  if (index >= lines.length) {
    return source
  }

  if (!/^#\s+.+$/.test(lines[index].trim())) {
    return source
  }

  index += 1

  while (index < lines.length && lines[index].trim() === '') {
    index += 1
  }

  return lines.slice(index).join('\n')
}

export function transformAdmonitions(source: string): string {
  const lines = source.split('\n')
  const output: string[] = []

  let insideBlock = false
  let blockType = ''
  let blockTitle = ''
  let blockContent: string[] = []

  for (const line of lines) {
    if (!insideBlock) {
      // 修复正则表达式，允许 :::
      const openMatch = line.match(/^:::\s*(\w+)(?:\s+(.*))?\s*$/)
      if (openMatch) {
        const candidateType = openMatch[1].toLowerCase()
        const mappedType = ADMONITION_TYPE_MAP[candidateType]
        if (mappedType) {
          insideBlock = true
          blockType = mappedType
          blockTitle = (openMatch[2] || '').trim()
          blockContent = []
          continue
        }
      }

      output.push(line)
      continue
    }

    if (/^:::\s*$/.test(line)) {
      const titleAttr = blockTitle
        ? ` title="${escapeAttrValue(blockTitle)}"`
        : ''

      output.push(`<Callout type="${blockType}"${titleAttr}>`)
      output.push('')
      output.push(...blockContent)
      output.push('')
      output.push('</Callout>')

      insideBlock = false
      blockType = ''
      blockTitle = ''
      blockContent = []
      continue
    }

    blockContent.push(line)
  }

  if (insideBlock) {
    const fallbackOpen = `:::${blockType}${blockTitle ? ` ${blockTitle}` : ''}`
    output.push(fallbackOpen)
    output.push(...blockContent)
  }

  return output.join('\n')
}

export function stripMdxSyntax(source: string): string {
  return source
    .replace(/```[\s\S]*?```/g, ' ')
    .replace(/`[^`]*`/g, ' ')
    .replace(/<[^>]+>/g, ' ')
    .replace(/!\[[^\]]*\]\([^)]*\)/g, ' ')
    .replace(/\[([^\]]+)\]\([^)]*\)/g, '$1')
    .replace(/^>\s?/gm, '')
    .replace(/[#*_~|>-]/g, ' ')
    .replace(/\s+/g, ' ')
    .trim()
}

export function extractExcerpt(source: string, query: string, maxLength = 180): string {
  const normalized = stripMdxSyntax(source)
  if (!normalized) {
    return ''
  }

  if (!query.trim()) {
    return normalized.slice(0, maxLength)
  }

  const lower = normalized.toLowerCase()
  const lowerQuery = query.toLowerCase().trim()
  const index = lower.indexOf(lowerQuery)

  if (index === -1) {
    return normalized.slice(0, maxLength)
  }

  const padding = Math.floor((maxLength - lowerQuery.length) / 2)
  const start = Math.max(0, index - padding)
  const end = Math.min(normalized.length, start + maxLength)
  const excerpt = normalized.slice(start, end).trim()

  if (start > 0 && end < normalized.length) {
    return `...${excerpt}...`
  }
  if (start > 0) {
    return `...${excerpt}`
  }
  if (end < normalized.length) {
    return `${excerpt}...`
  }

  return excerpt
}
