import type { MermaidRuntimeOptions } from './plugins/mermaid'

const ADMONITION_TYPE_MAP: Record<string, 'info' | 'warning' | 'danger' | 'success'> = {
  info: 'info',
  note: 'info',
  tip: 'success',
  details: 'info',
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
  let blockFenceLength = 3
  let blockRawType = ''
  let blockType = ''
  let blockTitle = ''
  let blockContent: string[] = []

  for (const line of lines) {
    if (!insideBlock) {
      const openMatch = line.match(/^(:{3,})\s*(\w+)(?:\s+(.*))?\s*$/)
      if (openMatch) {
        const fenceLength = openMatch[1].length
        const candidateType = openMatch[2].toLowerCase()
        const mappedType = ADMONITION_TYPE_MAP[candidateType]
        if (mappedType) {
          insideBlock = true
          blockFenceLength = fenceLength
          blockRawType = candidateType
          blockType = mappedType
          blockTitle = (openMatch[3] || '').trim()
          blockContent = []
          continue
        }
      }

      output.push(line)
      continue
    }

    const closeMatch = line.match(/^(:{3,})\s*$/)
    if (closeMatch && closeMatch[1].length >= blockFenceLength) {
      const titleAttr = blockTitle
        ? ` title=\"${escapeAttrValue(blockTitle)}\"`
        : ''

      output.push(`<Callout type=\"${blockType}\"${titleAttr}>`)
      output.push('')
      output.push(...blockContent)
      output.push('')
      output.push('</Callout>')

      insideBlock = false
      blockFenceLength = 3
      blockRawType = ''
      blockType = ''
      blockTitle = ''
      blockContent = []
      continue
    }

    blockContent.push(line)
  }

  if (insideBlock) {
    const fallbackOpen = `${':'.repeat(blockFenceLength)}${blockRawType}${blockTitle ? ` ${blockTitle}` : ''}`
    output.push(fallbackOpen)
    output.push(...blockContent)
  }

  return output.join('\n')
}

export function transformMermaidFences(source: string, config: MermaidRuntimeOptions): string {
  const lines = source.split('\n')
  const output: string[] = []

  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index]
    const openMatch = line.trim().match(/^([`~]{3,})\s*mermaid(?:\s+.*)?$/)
    if (!openMatch) {
      output.push(line)
      continue
    }

    const fence = openMatch[1]
    const marker = fence[0]
    const minLength = fence.length
    const diagramLines: string[] = []

    let cursor = index + 1
    let closed = false

    while (cursor < lines.length) {
      const maybeClose = lines[cursor].trim().match(/^([`~]{3,})\s*$/)
      if (maybeClose && maybeClose[1][0] === marker && maybeClose[1].length >= minLength) {
        closed = true
        break
      }
      diagramLines.push(lines[cursor])
      cursor += 1
    }

    if (!closed) {
      output.push(line, ...diagramLines)
      index = cursor - 1
      continue
    }

    const serializedChart = JSON.stringify(diagramLines.join('\n').trim())
    const serializedConfig = JSON.stringify(config)
    output.push(`<Mermaid chart={${serializedChart}} config={${serializedConfig}} />`)
    index = cursor
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
