# Rescue Xiaoan Repository Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build and publish a judge-facing `Rescue Xiaoan` repository that presents the project clearly, accepts technical contributions by subsystem, and keeps demonstration assets organized.

**Architecture:** Use one public GitHub repository with a judge-first root README. Technical source, standalone demonstrations, project documentation, showcase media, and submission copy are isolated in top-level directories. A lightweight Node test verifies required paths and repository hygiene before every push.

**Tech Stack:** Git, GitHub CLI, Markdown, standalone HTML/CSS/JavaScript, Node.js test runner

---

### Task 1: Repository contract test

**Files:**
- Create: `tests/repository-structure.test.mjs`

- [ ] **Step 1: Write the failing repository structure test**

Create a Node test that requires the judge-facing README, core documentation, contribution guide, technical landing page, demo landing page, showcase landing page, and submission description. It must also reject tracked model weights, environment files, private keys, caches, and files at or above GitHub's 100 MB hard limit.

- [ ] **Step 2: Run the test and confirm it fails because the repository files do not exist**

Run: `node --test tests/repository-structure.test.mjs`

Expected: FAIL with missing required paths such as `README.md`.

- [ ] **Step 3: Commit the failing contract test**

```bash
git add tests/repository-structure.test.mjs
git commit -m "test: define public repository contract"
```

### Task 2: Judge-facing repository foundation

**Files:**
- Create: `README.md`
- Create: `.gitignore`
- Create: `CONTRIBUTING.md`
- Create: `technical/README.md`
- Create: `demo/README.md`
- Create: `docs/project-overview.md`
- Create: `docs/architecture.md`
- Create: `docs/hardware.md`
- Create: `docs/technical-details.md`
- Create: `docs/limitations.md`
- Create: `showcase/README.md`
- Create: `submission/project-description.md`
- Create: `submission/pitch-script.md`
- Create: `submission/credits.md`

- [ ] **Step 1: Build the root README**

Lead with `Rescue Xiaoan`, the one-sentence outcome, the rescue problem, the system solution, demonstrated workflow, architecture summary, evidence links, and repository map. Do not lead with installation instructions or claim medical diagnosis.

- [ ] **Step 2: Add contribution and hygiene rules**

Document the five technical ownership areas and prohibit secrets, NDA-protected SDK files, local model weights, raw recordings, caches, and generated work files. Keep the repository public without assigning a software license until the owner confirms one.

- [ ] **Step 3: Add concise project documentation**

Use the confirmed rescue narrative: machine dog enters hazardous areas, Insta360 X4 supplies 360-degree visual evidence, RDK handles edge processing and integration, multimodal AI structures observable signs and environment risks, and the command center turns evidence into human-confirmed actions.

- [ ] **Step 4: Run the repository test**

Run: `node --test tests/repository-structure.test.mjs`

Expected: PASS.

- [ ] **Step 5: Commit the foundation**

```bash
git add README.md .gitignore CONTRIBUTING.md technical demo docs showcase submission
git commit -m "docs: create Rescue Xiaoan judge experience"
```

### Task 3: Import the existing rescue command-center UI

**Files:**
- Create: `demo/rescue-command-center/index.html`
- Create: `demo/rescue-command-center/assets/rescue/live-rescue.mp4`
- Create: `demo/rescue-command-center/assets/rescue/p01.png`
- Create: `demo/rescue-command-center/assets/rescue/p02.png`
- Create: `demo/rescue-command-center/assets/rescue/p03.png`
- Create: `demo/rescue-command-center/README.md`

- [ ] **Step 1: Add a failing demo asset test**

Extend `tests/repository-structure.test.mjs` to assert that the standalone UI and all four referenced media assets exist, and that the HTML uses only relative repository paths.

- [ ] **Step 2: Run the test and confirm the missing demo failure**

Run: `node --test tests/repository-structure.test.mjs`

Expected: FAIL for `demo/rescue-command-center/index.html`.

- [ ] **Step 3: Copy the verified UI and its runtime assets**

Copy the current `airshot-demo/rescue-demo.html` as `index.html` and copy only its four required rescue assets. Do not copy development environments, TTS models, temporary output, or obsolete photography Moment assets.

- [ ] **Step 4: Run the test and inspect the standalone page**

Run: `node --test tests/repository-structure.test.mjs`

Expected: PASS.

- [ ] **Step 5: Commit the demo**

```bash
git add demo/rescue-command-center tests/repository-structure.test.mjs
git commit -m "feat: add rescue command-center demonstration"
```

### Task 4: Publish the public GitHub repository

**Files:**
- Modify: Git remote configuration

- [ ] **Step 1: Verify local history and repository size**

Run: `git status --short --branch`

Expected: `## main` with no uncommitted files.

Run: `node --test tests/repository-structure.test.mjs`

Expected: PASS.

- [ ] **Step 2: Authenticate GitHub CLI if required**

Run: `gh auth status`

If the existing token is invalid, complete `gh auth login --hostname github.com --git-protocol https --web` in the user's GitHub session.

- [ ] **Step 3: Create and push the public repository**

Run: `gh repo create rescue-xiaoan --public --source . --remote origin --push --description "360-degree multimodal rescue robot dog command system"`

Expected: GitHub returns the new public repository URL and pushes `main`.

- [ ] **Step 4: Verify public access**

Run: `gh repo view --web` and `git ls-remote origin refs/heads/main`.

Expected: the public page opens and the remote `main` hash matches the local `HEAD` hash.
