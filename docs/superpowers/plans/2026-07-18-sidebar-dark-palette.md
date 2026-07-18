# Sidebar Dark Palette Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the Nereids Cascades guide sidebar dark and readable in both light and dark system color schemes without changing the main-content palette.

**Architecture:** Add sidebar-specific custom properties to the existing root palette and consume them only in sidebar selectors. Validate the CSS contract, WCAG text contrast, HTML parsing, and final diff without introducing runtime dependencies.

**Tech Stack:** HTML5, CSS custom properties, Python 3 standard library validation, Git

---

### Task 1: Isolate the sidebar palette

**Files:**
- Modify: `docs/nereids-cascades-framework-guide.html:10-32`
- Modify: `docs/nereids-cascades-framework-guide.html:92-123`

- [ ] **Step 1: Run a failing sidebar palette contract check**

Run:

```bash
python3 - <<'PY'
from pathlib import Path

html = Path("docs/nereids-cascades-framework-guide.html").read_text()
required = {
    "--sidebar-bg: #101c28;",
    "--sidebar-primary: #f4f8fb;",
    "--sidebar-text: #c7d5df;",
    "--sidebar-muted: #91a7b7;",
    "--sidebar-hover: #1d3445;",
    "--sidebar-border: #30495b;",
    "color: var(--sidebar-text);",
    "background: var(--sidebar-bg);",
    "color: var(--sidebar-primary);",
    "color: var(--sidebar-muted);",
    "background: var(--sidebar-hover);",
    "border: 1px solid var(--sidebar-border);",
}
missing = sorted(declaration for declaration in required if declaration not in html)
assert not missing, f"missing sidebar palette declarations: {missing}"
PY
```

Expected: FAIL with `AssertionError: missing sidebar palette declarations` because the dedicated variables do not exist yet.

- [ ] **Step 2: Define and apply the sidebar-specific colors**

Add these declarations after `--diagram-edge` in `:root`:

```css
      --sidebar-bg: #101c28;
      --sidebar-primary: #f4f8fb;
      --sidebar-text: #c7d5df;
      --sidebar-muted: #91a7b7;
      --sidebar-hover: #1d3445;
      --sidebar-border: #30495b;
```

Update only the sidebar selectors:

```css
    .sidebar {
      color: var(--sidebar-text);
      background: var(--sidebar-bg);
    }
    .side-brand strong { color: var(--sidebar-primary); }
    .side-brand span { color: var(--sidebar-muted); }
    .sidebar nav a { color: var(--sidebar-text); }
    .sidebar nav a:hover { color: var(--sidebar-primary); background: var(--sidebar-hover); }
    .sidebar .side-note {
      border: 1px solid var(--sidebar-border);
      color: var(--sidebar-muted);
    }
```

Retain all existing layout, spacing, typography, and responsive declarations around those properties.

- [ ] **Step 3: Re-run the palette contract check**

Run:

```bash
python3 - <<'PY'
from pathlib import Path

html = Path("docs/nereids-cascades-framework-guide.html").read_text()
required = {
    "--sidebar-bg: #101c28;",
    "--sidebar-primary: #f4f8fb;",
    "--sidebar-text: #c7d5df;",
    "--sidebar-muted: #91a7b7;",
    "--sidebar-hover: #1d3445;",
    "--sidebar-border: #30495b;",
    "color: var(--sidebar-text);",
    "background: var(--sidebar-bg);",
    "color: var(--sidebar-primary);",
    "color: var(--sidebar-muted);",
    "background: var(--sidebar-hover);",
    "border: 1px solid var(--sidebar-border);",
}
missing = sorted(declaration for declaration in required if declaration not in html)
assert not missing, f"missing sidebar palette declarations: {missing}"
PY
```

Expected: PASS with exit status 0 and no output.

- [ ] **Step 4: Verify sidebar text contrast**

Run:

```bash
python3 - <<'PY'
def luminance(color):
    channels = [int(color[index:index + 2], 16) / 255 for index in (1, 3, 5)]
    channels = [value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4 for value in channels]
    return 0.2126 * channels[0] + 0.7152 * channels[1] + 0.0722 * channels[2]

def contrast(foreground, background):
    lighter, darker = sorted((luminance(foreground), luminance(background)), reverse=True)
    return (lighter + 0.05) / (darker + 0.05)

checks = {
    "primary": ("#f4f8fb", "#101c28"),
    "navigation": ("#c7d5df", "#101c28"),
    "secondary": ("#91a7b7", "#101c28"),
    "hover": ("#f4f8fb", "#1d3445"),
}
for name, colors in checks.items():
    ratio = contrast(*colors)
    print(f"{name}: {ratio:.2f}:1")
    assert ratio >= 4.5, f"{name} contrast is below WCAG AA"
PY
```

Expected: all four ratios print at or above `4.50:1`, and the command exits successfully.

### Task 2: Validate and commit the HTML change

**Files:**
- Modify: `docs/nereids-cascades-framework-guide.html`

- [ ] **Step 1: Parse the complete HTML document**

Run:

```bash
python3 - <<'PY'
from html.parser import HTMLParser
from pathlib import Path

class DocumentParser(HTMLParser):
    pass

parser = DocumentParser()
parser.feed(Path("docs/nereids-cascades-framework-guide.html").read_text())
parser.close()
print("HTML parse: OK")
PY
```

Expected: `HTML parse: OK`.

- [ ] **Step 2: Check whitespace and review the scoped diff**

Run:

```bash
git diff --check -- docs/nereids-cascades-framework-guide.html
git diff -- docs/nereids-cascades-framework-guide.html
```

Expected: `git diff --check` produces no output; the diff contains only the six custom properties and sidebar color substitutions.

- [ ] **Step 3: Commit the HTML change**

Run:

```bash
git add docs/nereids-cascades-framework-guide.html
git commit
```

Use this commit title:

```text
[fix](fe) Improve Cascades guide sidebar contrast
```

The commit body must explain the dark-mode `--deep` variable collision, state that only the sidebar palette changes, and report the palette contract, contrast, HTML parse, and diff checks actually run.
