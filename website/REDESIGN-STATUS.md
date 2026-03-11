# Website Redesign Status

## Current State: In Progress

### Completed
- [x] Backed up original page → `index.backup.html` (3,383 lines)
- [x] New `index.html` built (1,691 lines — 50% smaller)
- [x] Removed entire firmware flasher (ESP Web Tools, serial terminal, diagnostics, FAQ)
- [x] Nav with version pill badge (auto-populated from releases.json)
- [x] Hero with "Get Started" CTA, version badge, capability stats
- [x] 6 capability cards with SVG icons (replaces old feature cards + emojis)
- [x] Photo gallery with filters + lightbox (same 12 photos)
- [x] Use case cards with SVG icons (Bedside, School Nurse, Clinic)
- [x] #WeAreNotWaiting story section
- [x] DIY build guide (emojis kept, same Amazon links)
- [x] About the Developer section (placeholder content)
- [x] Firmware info strip with expandable release notes
- [x] Footer updated (Firmware link → Developer link)
- [x] Old photos restored from server

### Waiting On
- [ ] New device photos — Carl is taking new photos to add to `website/photos/`
- [ ] Update gallery array with new photos
- [x] Fill in About the Developer section — Carl Brothers, wife Stefanie, step-daughter with T1D inspiration, live entertainment industry background
- [x] Avatar photo: `photos/carlb.jpg` (Carl and Stefanie, circular crop)
- [x] Medical disclaimer added above footer
- [ ] Final review pass after photos are added
- [ ] Social links in Developer section (GitHub URL, Contact URL)

### Design Direction
- **Base**: Tech Product Launch (C) with Clinical Precision polish (A)
- **Color scheme**: Dark navy `#0a0e17` + electric cyan `#00d4ff` (unchanged)
- **Icons**: SVG everywhere except DIY parts list (emojis kept there)
- **Focus**: Capabilities over hardware specs
- **Tone**: Professional, confident, not childish/cartoony

### Key Files
- `website/index.html` — New landing page
- `website/index.backup.html` — Original with flasher (backup)
- `website/releases.json` — Firmware version data (unchanged)
- `website/photos/` — Gallery photos
- `docs/superpowers/specs/2026-03-11-website-redesign-design.md` — Design spec
