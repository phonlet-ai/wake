# Phonlet brand assets

Phonlet logos, artwork and design tokens used by the README, the docs and the browser demo. [manifest.json](manifest.json) records each file's SHA-256; the repository gate checks them.

| Asset | Use here |
| --- | --- |
| [Wake cover](phonlet-ai-wake-cover.svg) | Main repository README |
| [Light logo](phonlet-ai-horizontal-light.svg) | Documentation on light backgrounds |
| [Dark logo](phonlet-ai-horizontal-dark.svg) | Documentation and demo on dark backgrounds |
| [Micro favicon](phonlet-ai-favicon-gradient.svg) | Browser demo tab |
| [Wave background](phonlet-ai-wave-background.svg) | Decorative demo hero artwork |
| [CSS tokens](brand-tokens.css) | Official palette, typography fallbacks, spacing and radii |

Use the supplied logo geometry and gradient. Preserve aspect ratio, leave clear space around the visible artwork, and use the light/dark variant appropriate to the surface. Horizontal logos should be at least 180 px wide. The wave artwork is decorative, not a measured audio signal. Full logos use the alternative text “Phonlet AI.”

The interface uses the kit's system-font fallback; no font files or external font service are bundled. The browser build copies its required assets into the standalone demo. SDK/model packages do not carry decorative artwork.

The Phonlet name and logos identify Phonlet AI. The repository's [licence](../../LICENSE) covers the code and model; it grants no trademark rights, so do not use the logos to suggest that your product is made or endorsed by Phonlet AI.
