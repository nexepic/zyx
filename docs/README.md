# Metrix Documentation

This directory contains the VitePress documentation for the Metrix graph database project.

## Local Development

### Prerequisites

- Node.js 20+
- pnpm 9+

### Install Dependencies

```bash
pnpm install
```

### Development Server

```bash
pnpm run dev
```

The site will be available at `http://localhost:5173`

### Build

```bash
pnpm run build
```

The built site will be in `.vitepress/dist/`

### Preview Build

```bash
pnpm run preview
```

## Languages

- **English**: `/en/` (primary)
- **Chinese**: `/zh/` (secondary)

## Deployment

Documentation is automatically deployed to GitHub Pages when changes are pushed to the `main` branch.

## Adding Content

1. Create or edit markdown files in `en/` or `zh/`
2. Update sidebar configuration in `.vitepress/config/en.ts` or `.vitepress/config/zh.ts`
3. Test locally with `pnpm run dev`
4. Commit and push to trigger deployment
