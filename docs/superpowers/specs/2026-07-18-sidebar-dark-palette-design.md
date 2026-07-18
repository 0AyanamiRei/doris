# Sidebar Dark Palette Design

## Problem

The desktop sidebar uses `var(--deep)` as its background. In dark color-scheme mode, `--deep` becomes a light heading color, so the sidebar renders with a pale background while retaining pale navigation text. This makes the navigation difficult to read.

## Scope

Change only the sidebar colors in `docs/nereids-cascades-framework-guide.html`. Preserve the document content, layout, responsive behavior, print output, and main-content palette.

## Design

Define dedicated sidebar variables in `:root` so the sidebar no longer shares the semantic heading color:

- Background: `#101c28`
- Primary text: `#f4f8fb`
- Navigation text: `#c7d5df`
- Secondary text: `#91a7b7`
- Hover background: `#1d3445`
- Border: `#30495b`

Use the same dark sidebar palette in light and dark color schemes. Apply the variables to the sidebar background, brand text, navigation links, hover state, note text, and note border. Keep the existing focus treatment.

## Validation

- Confirm the sidebar selectors no longer depend on `--deep`.
- Calculate WCAG contrast ratios for normal, secondary, and hover text against their backgrounds; normal-sized text must meet at least 4.5:1.
- Parse or render the local HTML to catch structural or CSS regressions.
- Review the final diff to ensure only sidebar styling and its design record changed.
