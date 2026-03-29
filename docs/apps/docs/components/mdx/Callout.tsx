'use client'

import { useState } from 'react'
import { AlertCircle, AlertTriangle, CheckCircle2, Info, ChevronDown, ChevronRight } from 'lucide-react'
import { cn } from '@/lib/utils'

type CalloutType = 'info' | 'warning' | 'danger' | 'success' | 'note' | 'tip'

interface CalloutProps {
  type?: CalloutType
  title?: string
  compact?: boolean
  children: React.ReactNode
}

const typeMap: Record<CalloutType, 'info' | 'warning' | 'danger' | 'success'> = {
  info: 'info',
  note: 'info',
  warning: 'warning',
  danger: 'danger',
  success: 'success',
  tip: 'success',
}

const styles = {
  info: {
    icon: Info,
    bg: 'bg-[#58a6ff]/[0.06]',
    borderColor: 'border-[#58a6ff]/15',
    iconColor: 'text-[#58a6ff]',
    titleColor: 'text-[#58a6ff]',
  },
  warning: {
    icon: AlertTriangle,
    bg: 'bg-[#d29922]/[0.06]',
    borderColor: 'border-[#d29922]/15',
    iconColor: 'text-[#d29922]',
    titleColor: 'text-[#d29922]',
  },
  danger: {
    icon: AlertCircle,
    bg: 'bg-[#f85149]/[0.06]',
    borderColor: 'border-[#f85149]/15',
    iconColor: 'text-[#f85149]',
    titleColor: 'text-[#f85149]',
  },
  success: {
    icon: CheckCircle2,
    bg: 'bg-[#3fb950]/[0.06]',
    borderColor: 'border-[#3fb950]/15',
    iconColor: 'text-[#3fb950]',
    titleColor: 'text-[#3fb950]',
  },
}

const defaultTitles = { info: 'Note', warning: 'Warning', danger: 'Danger', success: 'Tip' }

export function Callout({ type = 'info', title, compact: initialCompact, children }: CalloutProps) {
  const normalizedType = typeMap[type]
  const style = styles[normalizedType]
  const Icon = style.icon
  const [isCompact, setIsCompact] = useState(initialCompact ?? false)

  return (
    <aside
      className={cn(
        'relative my-5 overflow-hidden rounded-lg border',
        style.bg,
        style.borderColor,
      )}
    >
      <div className="px-4 py-3.5">
        <div className="flex items-center justify-between">
          <div className="flex items-center gap-2">
            <Icon className={cn('h-[16px] w-[16px] flex-shrink-0', style.iconColor)} />
            <span className={cn('text-[13px] font-medium', style.titleColor)}>
              {title || defaultTitles[normalizedType]}
            </span>
          </div>
          <button
            type="button"
            onClick={() => setIsCompact((v) => !v)}
            className="inline-flex h-5 w-5 items-center justify-center rounded text-[var(--text-tertiary)] opacity-30 transition-opacity hover:opacity-70"
            aria-label={isCompact ? 'Expand' : 'Collapse'}
          >
            {isCompact ? (
              <ChevronRight className="h-3.5 w-3.5" />
            ) : (
              <ChevronDown className="h-3.5 w-3.5" />
            )}
          </button>
        </div>

        {!isCompact && (
          <div className="callout-content mt-2 text-[14px] leading-relaxed text-[var(--text-secondary)]">
            {children}
          </div>
        )}
      </div>
    </aside>
  )
}
