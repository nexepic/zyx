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

Static site assets live in:

- `apps/docs/public/assets/brand`
- `apps/docs/public/assets/icons`
- `apps/docs/public/assets/social`

Project-specific CSS overrides:

- `apps/docs/app/custom.css`

Project-specific homepage overrides:

- `apps/docs/home/custom-home.tsx`
