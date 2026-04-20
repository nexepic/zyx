const createMDX = require('@next/mdx')
const { withContentCollections } = require('@content-collections/next')
const withNextIntl = require('next-intl/plugin')('./i18n.ts')

const basePath = process.env.BASE_PATH || '/zyx'

/** @type {import('next').NextConfig} */
const nextConfig = {
  output: 'export',
  reactStrictMode: true,
  pageExtensions: ['js', 'jsx', 'mdx', 'ts', 'tsx'],
  transpilePackages: ['@nexdoc/core'],
  basePath,
  assetPrefix: basePath || undefined,

  // Static export configuration
  images: {
    unoptimized: true,
  },

  // Ensure proper trailing slash behavior for static hosting
  trailingSlash: true,

  // Expose basePath to client-side code
  env: {
    NEXT_PUBLIC_BASE_PATH: basePath,
  },
}

const withMDX = createMDX({
  extension: /\.mdx?$/,
})

module.exports = withContentCollections(withNextIntl(withMDX(nextConfig)))
