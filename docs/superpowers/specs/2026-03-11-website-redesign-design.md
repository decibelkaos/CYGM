# CYGM Website Redesign — Landing Page Without Flasher

## Summary

Redesign `website/index.html` as a modern, professional landing page for public launch. Remove the entire firmware flasher (OTA handles updates now). Keep the dark medical-device aesthetic with electric cyan accents. Direction: Tech Product Launch (C) with Clinical Precision polish (A).

## Design Direction

- **Base**: Tech Product Launch — spec grids, gradient accents, grid background, card-heavy, technical credibility with polish
- **Polish from Clinical Precision**: Version pill badge, clean thin typography accents, generous whitespace, surgical refinement
- **Tone**: Professional, confident, capability-focused. Not childish/cartoony.
- **Icons**: SVG icons everywhere except DIY parts list (emojis kept there)
- **Mobile**: Must work on all mobile devices. Responsive throughout.

## Color Scheme (Unchanged)

```css
--bg-primary: #0a0e17;
--bg-secondary: #0f1420;
--bg-tertiary: #151b2b;
--bg-card: #141a2a;
--accent: #00d4ff;
--accent-dim: #00a8cc;
--glucose-green: #22c55e;
--glucose-yellow: #eab308;
--glucose-red: #ef4444;
--text-primary: #e8ecf4;
--text-secondary: #8b95a8;
--text-muted: #505c72;
```

## Fonts (Unchanged)

- Outfit: Headlines
- DM Sans: Body
- JetBrains Mono: Labels, version badges, monospace elements

## Page Structure (10 Sections)

### 1. Navigation (Refined)
- Glassmorphic sticky nav with blur on scroll
- Logo with pulsing cyan dot
- Links: Features, Gallery, Build, About
- Version pill badge (from releases.json): `v1.7.0 Beta`
- "Get Started" as nav CTA (scrolls to Build section)
- Mobile hamburger menu

### 2. Hero (Refined)
- Version badge pill at top: `v1.7.0 Beta — Now with OTA Updates`
- Headline: capability-focused, bold (Outfit 800)
- Subtext: 1-2 sentences describing what CYGM does
- CTAs: "Get Started" (primary, scrolls to Build) + "View Gallery" (secondary)
- Stats row: ~$40 / Real-time / OTA Updates
- Device photo on right (desktop), below (mobile)
- Subtle grid background + ambient glow

### 3. What It Can Do (Changed — capability-focused)
- Section label: "Capabilities"
- 6 cards in 3x2 grid (1 col mobile)
- Each card: SVG icon + title + 1-2 line description
- Cards:
  1. Live Glucose Readings — real-time from Dexcom
  2. Trend Arrows & Charts — 1-24h trend graphs
  3. Smart Alarms — configurable high/low with snooze
  4. Weather & Time — local weather, clock, date
  5. OTA Firmware Updates — wireless updates
  6. Touchscreen Interface — full touch UI

### 4. Gallery (Kept)
- Same 12 photos array
- Same filter tabs: All, Dashboard, Graph, Settings
- Same lightbox with click-to-enlarge
- Scroll-reveal animations

### 5. Use Cases (Icons swapped)
- Same 3 cards: Bedside Monitor, School Nurse, Clinic
- SVG icons replace emojis
- Same copy

### 6. #WeAreNotWaiting Story (Kept)
- Two-column layout (photo + story)
- #WeAreNotWaiting hashtag badge
- Minor styling alignment

### 7. Build Your Own (Kept with changes)
- "Get Started" CTA scroll target
- Same 3-part BOM with emojis (kept here)
- Same Amazon links and prices
- "Coming soon" tooltips on supplementary links
- Card styling aligned with new direction

### 8. About the Developer (NEW)
- Circular avatar placeholder with accent border
- Name placeholder
- Generic bio text (to be replaced later)
- Social/contact pill links: GitHub, Twitter, Contact

### 9. Current Firmware (NEW mini-section)
- Elegant horizontal strip above footer
- Shows: version, stage badge, date, "Release notes" link
- Dynamically loaded from releases.json
- Informational only — no flasher

### 10. Footer (Kept)
- CYGM branding, copyright 2026
- Nav links (Features, Build, Developer, About)
- #WeAreNotWaiting

## Removed

- **Entire Firmware Flasher** (~1,200 lines): CSS, JS, HTML, ESP Web Tools import, serial terminal, progress bar, diagnostics, boot instructions, FAQ, flasher modal
- **ESP Web Tools script tag** from `<head>`
- **esptool-js dynamic import**

## Technical Notes

- Single self-contained HTML file (same as current)
- Version info loaded from `releases.json` (same fetch pattern)
- All SVG icons inline (no external icon library)
- Scroll-reveal via IntersectionObserver (same pattern)
- Gallery lightbox (same JS)
- ~1,800-2,000 lines target (down from 3,383)

## Files

- **Backup**: `website/index.html` → `website/index.backup.html`
- **Output**: New `website/index.html`
- **Unchanged**: `releases.json`, `version.json`, `cygm-manifest.json`, `photos/`
