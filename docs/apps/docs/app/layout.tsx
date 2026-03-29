import type { Metadata, Viewport } from 'next'
import './globals.css'
import { generateSiteMetadata } from '@/lib/metadata'

export const metadata: Metadata = generateSiteMetadata({
  title: 'Documentation',
  description: 'Professional documentation system for multiple projects.',
  pathname: '/',
})

export const viewport: Viewport = {
  themeColor: '#0b0f14',
  width: 'device-width',
  initialScale: 1,
}

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en" suppressHydrationWarning>
      <body className="app-shell">{children}</body>
    </html>
  )
}
