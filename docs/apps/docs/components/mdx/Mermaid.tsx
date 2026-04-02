'use client'

import { useEffect, useId, useRef, useState } from 'react'
import type { MermaidRuntimeOptions } from '@/lib/plugins/mermaid'

interface MermaidProps {
  chart: string
  config?: MermaidRuntimeOptions
}

function getTheme(config: MermaidRuntimeOptions): string {
  return document.documentElement.dataset.theme === 'dark' ? config.theme.dark : config.theme.light
}

export function Mermaid({ chart, config }: MermaidProps) {
  const containerRef = useRef<HTMLDivElement>(null)
  const [error, setError] = useState<string | null>(null)
  const id = useId().replace(/:/g, '-')

  useEffect(() => {
    let active = true
    const runtimeConfig: MermaidRuntimeOptions = config || {
      theme: {
        light: 'default',
        dark: 'dark',
      },
      securityLevel: 'loose',
    }

    const render = async () => {
      if (!containerRef.current) {
        return
      }

      try {
        const { default: mermaid } = await import('mermaid')
        mermaid.initialize({
          startOnLoad: false,
          theme: getTheme(runtimeConfig) as any,
          securityLevel: runtimeConfig.securityLevel,
        })

        const graphId = `mermaid-${id}-${Date.now()}`
        const { svg } = await mermaid.render(graphId, chart)
        if (!active || !containerRef.current) {
          return
        }

        containerRef.current.innerHTML = svg
        setError(null)
      } catch (nextError) {
        if (!active) {
          return
        }
        setError(nextError instanceof Error ? nextError.message : 'Failed to render Mermaid diagram.')
      }
    }

    const observer = new MutationObserver((records) => {
      if (records.some((record) => record.attributeName === 'data-theme')) {
        void render()
      }
    })

    observer.observe(document.documentElement, {
      attributes: true,
      attributeFilter: ['data-theme'],
    })

    void render()

    return () => {
      active = false
      observer.disconnect()
    }
  }, [chart, config, id])

  if (error) {
    return (
      <pre className="my-6 overflow-x-auto rounded-md border border-[var(--border)] bg-[var(--bg-code)] p-4 text-sm text-[var(--text-secondary)]">
        Mermaid render error: {error}
      </pre>
    )
  }

  return <div ref={containerRef} className="my-6 overflow-x-auto" />
}
