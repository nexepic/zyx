'use client'

import { useEffect, useRef } from 'react'
import mermaid from 'mermaid'

interface MermaidProps {
  chart: string
}

export function Mermaid({ chart }: MermaidProps) {
  const ref = useRef<HTMLDivElement>(null)

  useEffect(() => {
    mermaid.initialize({
      startOnLoad: false,
      theme: 'neutral',
      securityLevel: 'loose',
    })

    if (ref.current) {
      mermaid.run({
        nodes: [ref.current],
      })
    }
  }, [chart])

  return (
    <div
      ref={ref}
      className="mermaid my-6 flex justify-center"
    >
      {chart}
    </div>
  )
}
