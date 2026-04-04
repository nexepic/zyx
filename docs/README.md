# NexDoc Site

Documentation site scaffolded with **NexDoc**.

## Development

```bash
bun install
bun run dev
```

Open:

- `http://localhost:3000/en`
- `http://localhost:3000/zh`

## Build

```bash
bun run build
bun run start
```

Clear local caches:

```bash
bun run clean
```

## Generated Caches

Local build caches are generated automatically and should stay untracked:

- `apps/docs/.content-collections/`
- `apps/docs/.next/`
- `apps/docs/.swc/`

## Content

Write docs in:

- `apps/docs/content/docs/en/*.mdx`
- `apps/docs/content/docs/zh/*.mdx`

## Site Config Highlights

Edit `apps/docs/nexdoc.config.ts` for homepage SEO metadata and icon metadata behavior:

```ts
home: {
  metadata: {
    en: {
      title: 'Documentation',
      description: 'Everything you need to get started, integrate, and build with confidence.',
    },
    zh: {
      title: '文档',
      description: '快速上手、集成和构建所需的一切。',
    },
    default: {
      title: 'Documentation',
      description: 'Product documentation',
    },
  },
},
assets: {
  brand: {
    icon: '/assets/brand/icon.svg',
    iconLight: '/assets/brand/icon-light.svg', // optional metadata fallback
    iconDark: '/assets/brand/icon-dark.svg', // optional metadata fallback
  },
  icons: {
    icon: '/assets/icons/icon.svg', // optional
    favicon: '/assets/icons/favicon.svg',
    shortcut: '/assets/icons/favicon.ico', // optional
    apple: '/assets/icons/apple-touch-icon.png', // optional
  },
}
```

Metadata icon fallback:

- If any `assets.icons.*` is set, metadata icon links use explicit `assets.icons` values.
- If `assets.icons` is not set, metadata can fall back to `assets.brand.iconLight` and `assets.brand.iconDark`.

Static site assets live in:

- `apps/docs/public/assets/brand`
- `apps/docs/public/assets/icons`
- `apps/docs/public/assets/social`

Project-specific CSS overrides:

- `apps/docs/app/custom.css`

Project-specific homepage overrides:

- `apps/docs/home/custom-home.tsx`
