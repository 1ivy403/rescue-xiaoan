# Rescue Xiaoan Repository Design

## Purpose

`Rescue Xiaoan` is the public submission repository for hackathon judges. It is designed for review rather than local deployment: a judge should quickly understand the problem, solution, completed prototype, technical evidence, and demonstration results.

## Information Order

The repository follows this reading path:

1. Understand the project from the root `README.md`.
2. Watch the field scenario, command-center UI, and live technical demonstration.
3. Inspect the architecture and hardware responsibilities.
4. Review the technical source code by subsystem.
5. Open the standalone UI demonstrations and submission materials.

## Repository Boundaries

```text
rescue-xiaoan/
├── README.md
├── technical/
│   ├── camera-stream/
│   ├── multimodal-ai/
│   ├── rdk-edge/
│   ├── robot-control/
│   └── system-integration/
├── demo/
│   ├── rescue-command-center/
│   └── live-tech-dashboard/
├── docs/
│   ├── project-overview.md
│   ├── architecture.md
│   ├── hardware.md
│   ├── technical-details.md
│   └── limitations.md
├── showcase/
│   ├── videos/
│   ├── screenshots/
│   ├── poster/
│   └── ui-assets/
└── submission/
    ├── project-description.md
    ├── pitch-script.md
    └── credits.md
```

## Content Rules

- The root README leads with the project outcome, not installation instructions.
- Technical code is grouped by responsibility so contributors can push modules independently.
- Demonstration HTML and its runtime assets stay together under `demo/`.
- Posters, screenshots, and videos stay under `showcase/` and are optimized for direct GitHub viewing.
- Research drafts from the earlier photography concept are excluded to avoid confusing the final rescue narrative.
- Medical claims are limited to observable audio and visual signs; the system does not claim clinical diagnosis.

## Large Files And Generated Output

The repository excludes virtual environments, caches, temporary editing output, raw recordings, generated work files, and local model weights. The 160 MB Kokoro model is not committed. Final compressed demonstration videos may be committed while they remain below GitHub's per-file limit; larger media should use Git LFS or a release attachment.

## Licensing

The repository is public, but no software license is assigned until the project owner confirms one. Third-party models, SDKs, and open-source components must retain their original notices and must not expose NDA-protected Insta360 SDK files.
