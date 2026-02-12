#!/usr/bin/env python3
"""Extract doc comments from Lattice C source and generate docs.html"""

import os
import re
import sys
import html
from pathlib import Path
from collections import OrderedDict


# ── Category ordering and grouping ──────────────────────────────────────────

SECTION_GROUPS = OrderedDict([
    ("Language", [
        "Core",
        "Phase Transitions",
        "Type Constructors",
        "Type Conversion",
        "Error Handling",
    ]),
    ("Standard Library", [
        "Math",
        "String Formatting",
        "Regex",
        "JSON",
        "CSV",
        "URL",
        "Functional",
        "Metaprogramming",
    ]),
    ("System", [
        "File System",
        "Path",
        "Environment",
        "Process",
        "Date & Time",
        "Crypto",
        "Networking",
    ]),
    ("Type Methods", [
        "String Methods",
        "Array Methods",
        "Map Methods",
        "Channel Methods",
    ]),
])

# Flat order of all categories for sorting
CATEGORY_ORDER = []
for cats in SECTION_GROUPS.values():
    CATEGORY_ORDER.extend(cats)


# ── Doc comment parser ──────────────────────────────────────────────────────

class DocEntry:
    def __init__(self):
        self.kind = ""        # "builtin" or "method"
        self.signature = ""   # e.g. "function_name(param: Type) -> ReturnType"
        self.name = ""        # e.g. "function_name" or "String.method_name"
        self.category = ""
        self.description = ""
        self.examples = []    # each instance gets its own list via __init__


def parse_doc_blocks(source_text):
    """Parse all /// doc comment blocks from a C source string."""
    entries = []
    lines = source_text.split("\n")
    i = 0

    while i < len(lines):
        line = lines[i].strip()

        # Look for start of a doc block: /// @builtin or /// @method
        if line.startswith("///") and ("@builtin" in line or "@method" in line):
            entry = DocEntry()
            # Parse the first line
            content = line[3:].strip()  # strip "/// "

            if "@builtin" in content:
                entry.kind = "builtin"
                entry.signature = content.replace("@builtin", "").strip()
            elif "@method" in content:
                entry.kind = "method"
                entry.signature = content.replace("@method", "").strip()

            # Extract name from signature (everything before the first '(')
            paren_idx = entry.signature.find("(")
            if paren_idx >= 0:
                entry.name = entry.signature[:paren_idx].strip()
            else:
                entry.name = entry.signature.strip()

            # Continue reading subsequent /// lines
            i += 1
            desc_lines = []
            while i < len(lines):
                next_line = lines[i].strip()
                if not next_line.startswith("///"):
                    break

                content = next_line[3:].strip()

                if content.startswith("@category"):
                    entry.category = content.replace("@category", "").strip()
                elif content.startswith("@example"):
                    example_text = content.replace("@example", "").strip()
                    entry.examples.append(example_text)
                else:
                    # Description line
                    if content:
                        desc_lines.append(content)
                i += 1

            entry.description = " ".join(desc_lines)
            entries.append(entry)
        else:
            i += 1

    return entries


def scan_source_files(src_dir):
    """Scan all .c files in src_dir for doc comments."""
    entries = []
    src_path = Path(src_dir)

    if not src_path.exists():
        print(f"Warning: source directory '{src_dir}' does not exist", file=sys.stderr)
        return entries

    c_files = sorted(src_path.glob("*.c"))
    for c_file in c_files:
        try:
            text = c_file.read_text(encoding="utf-8", errors="replace")
            file_entries = parse_doc_blocks(text)
            entries.extend(file_entries)
        except Exception as e:
            print(f"Warning: could not read {c_file}: {e}", file=sys.stderr)

    return entries


def group_by_category(entries):
    """Group entries by category, maintaining defined order."""
    groups = OrderedDict()

    # Initialize in defined order
    for cat in CATEGORY_ORDER:
        groups[cat] = []

    # Place entries
    for entry in entries:
        cat = entry.category if entry.category else "Core"
        if cat not in groups:
            groups[cat] = []
        groups[cat].append(entry)

    # Remove empty categories
    return OrderedDict((k, v) for k, v in groups.items() if v)


# ── HTML generation ─────────────────────────────────────────────────────────

def escape(text):
    """HTML-escape a string."""
    return html.escape(text, quote=True)


def format_signature_html(sig):
    """Render a function signature with syntax coloring."""
    # Parse: name(params) -> ReturnType
    # Or: Type.name(params) -> ReturnType
    m = re.match(r'^([\w:.]+)\s*\((.*?)\)\s*(->\s*(.+))?$', sig, re.DOTALL)
    if not m:
        return f'<span class="fn">{escape(sig)}</span>'

    name = m.group(1)
    params_raw = m.group(2)
    ret_arrow = m.group(3) or ""
    ret_type = m.group(4) or ""

    parts = []
    parts.append(f'<span class="fn">{escape(name)}</span>')
    parts.append('<span class="text">(</span>')

    # Parse parameters: "param: Type, param2: Type"
    if params_raw.strip():
        param_list = [p.strip() for p in params_raw.split(",")]
        formatted_params = []
        for param in param_list:
            colon_idx = param.find(":")
            if colon_idx >= 0:
                pname = param[:colon_idx].strip()
                ptype = param[colon_idx+1:].strip()
                formatted_params.append(
                    f'<span class="text">{escape(pname)}</span>'
                    f'<span class="op">:</span> '
                    f'<span class="typ">{escape(ptype)}</span>'
                )
            else:
                formatted_params.append(f'<span class="text">{escape(param)}</span>')
        parts.append('<span class="op">, </span>'.join(formatted_params))

    parts.append('<span class="text">)</span>')

    if ret_type.strip():
        parts.append(f' <span class="op">-&gt;</span> <span class="typ">{escape(ret_type.strip())}</span>')

    return "".join(parts)


def format_example_html(example):
    """Render an example with comment highlighting."""
    # Split on // to highlight the comment part
    comment_idx = example.find("//")
    if comment_idx >= 0:
        code_part = escape(example[:comment_idx])
        comment_part = escape(example[comment_idx:])
        return f'{code_part}<span class="cmt">{comment_part}</span>'
    return escape(example)


def make_anchor(name):
    """Create a URL-safe anchor from a name."""
    return re.sub(r'[^a-zA-Z0-9_-]', '-', name).lower().strip('-')


def generate_html(groups, output_path):
    """Generate the complete docs.html file."""

    # Build sidebar HTML
    sidebar_html = ""
    for section_name, categories in SECTION_GROUPS.items():
        active_cats = [c for c in categories if c in groups]
        if not active_cats:
            continue
        sidebar_html += f'      <div class="sidebar-section">\n'
        sidebar_html += f'        <div class="sidebar-heading">{escape(section_name)}</div>\n'
        for cat in active_cats:
            anchor = make_anchor(cat)
            count = len(groups[cat])
            sidebar_html += (
                f'        <a href="#{anchor}" class="sidebar-link" data-cat="{anchor}">'
                f'{escape(cat)}<span class="sidebar-count">{count}</span></a>\n'
            )
        sidebar_html += f'      </div>\n'

    # Build content HTML
    content_html = ""
    for section_name, categories in SECTION_GROUPS.items():
        active_cats = [c for c in categories if c in groups]
        if not active_cats:
            continue

        for cat in active_cats:
            anchor = make_anchor(cat)
            entries = groups[cat]
            content_html += f'    <div class="doc-category" id="{anchor}">\n'
            content_html += f'      <div class="section-label">{escape(section_name)}</div>\n'
            content_html += f'      <h2 class="section-title">{escape(cat)}</h2>\n'

            for entry in entries:
                entry_anchor = make_anchor(f"fn-{entry.name}")
                sig_html = format_signature_html(entry.signature)
                content_html += f'      <div class="doc-entry" id="{entry_anchor}" data-name="{escape(entry.name.lower())}" data-desc="{escape(entry.description.lower())}">\n'
                content_html += f'        <div class="doc-sig">{sig_html}</div>\n'
                if entry.description:
                    content_html += f'        <p class="doc-desc">{escape(entry.description)}</p>\n'
                if entry.examples:
                    content_html += f'        <div class="doc-examples">\n'
                    for ex in entry.examples:
                        content_html += f'          <pre><code>{format_example_html(ex)}</code></pre>\n'
                    content_html += f'        </div>\n'
                content_html += f'      </div>\n'

            content_html += f'    </div>\n'

    # Handle empty state
    if not groups:
        content_html = """    <div class="doc-empty">
      <div class="doc-empty-icon">&#x25C7;</div>
      <h2>No Documentation Yet</h2>
      <p>Add <code>/// @builtin</code> or <code>/// @method</code> doc comments to C source files to generate documentation.</p>
      <div class="doc-empty-example">
        <pre><code><span class="cmt">/// @builtin print(value: Any) -> Unit</span>
<span class="cmt">/// @category Core</span>
<span class="cmt">/// Prints a value to stdout followed by a newline.</span>
<span class="cmt">/// @example print("hello")  // hello</span>
if (strcmp(fn_name, "print") == 0) {</code></pre>
      </div>
    </div>
"""
        sidebar_html = '      <div class="sidebar-empty">Add doc comments to source files to see categories here.</div>\n'

    # Count total entries
    total_entries = sum(len(v) for v in groups.values())
    total_categories = len(groups)

    full_html = f"""<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <!-- Google tag (gtag.js) -->
  <script async src="https://www.googletagmanager.com/gtag/js?id=G-1YKVFGCL3K"></script>
  <script>
    window.dataLayer = window.dataLayer || [];
    function gtag(){{dataLayer.push(arguments);}}
    gtag('js', new Date());
    gtag('config', 'G-1YKVFGCL3K');
  </script>
  <title>Documentation — Lattice</title>
  <meta name="description" content="API documentation for the Lattice programming language. Browse builtins, type methods, and standard library functions.">
  <link rel="preconnect" href="https://fonts.googleapis.com">
  <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
  <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&display=swap" rel="stylesheet">
  <style>
    /* ── Reset & Base ── */
    *, *::before, *::after {{ box-sizing: border-box; margin: 0; padding: 0; }}

    :root {{
      --bg: #08080d;
      --bg-card: #0e0e18;
      --border: #1a1a2e;
      --text: #c8c8d4;
      --text-dim: #6a6a80;
      --heading: #e8e8f0;
      --accent: #4fc3f7;
      --accent-dim: #2a7ea8;
      --keyword: #c792ea;
      --string: #c3e88d;
      --comment: #546e7a;
      --type: #ffcb6b;
      --number: #f78c6c;
      --fn: #82aaff;
      --mono: 'SF Mono', 'Cascadia Code', 'JetBrains Mono', 'Fira Code', Consolas, monospace;
      --sans: 'Inter', -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif;
    }}

    html {{ scroll-behavior: smooth; height: 100%; }}

    body {{
      font-family: var(--sans);
      background: var(--bg);
      color: var(--text);
      line-height: 1.7;
      height: 100%;
      overflow: hidden;
      display: flex;
      flex-direction: column;
    }}

    /* ── Lattice background pattern ── */
    body::before {{
      content: '';
      position: fixed;
      inset: 0;
      background:
        linear-gradient(rgba(79,195,247,0.03) 1px, transparent 1px),
        linear-gradient(90deg, rgba(79,195,247,0.03) 1px, transparent 1px);
      background-size: 60px 60px;
      pointer-events: none;
      z-index: 0;
    }}

    /* ── Header ── */
    .doc-header {{
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 12px 24px;
      border-bottom: 1px solid var(--border);
      background: var(--bg-card);
      position: relative;
      z-index: 10;
      flex-shrink: 0;
    }}

    .doc-header-left {{
      display: flex;
      align-items: center;
      gap: 16px;
    }}

    .doc-logo {{
      font-family: var(--mono);
      font-size: 1rem;
      font-weight: 700;
      color: var(--heading);
      text-decoration: none;
      background: linear-gradient(135deg, var(--heading), var(--accent));
      -webkit-background-clip: text;
      -webkit-text-fill-color: transparent;
      background-clip: text;
    }}

    .doc-sep {{
      color: var(--border);
      font-size: 1.2rem;
      user-select: none;
    }}

    .doc-title {{
      font-family: var(--mono);
      font-size: 0.85rem;
      color: var(--text-dim);
    }}

    .doc-header-right {{
      display: flex;
      align-items: center;
      gap: 12px;
    }}

    .doc-btn {{
      font-family: var(--mono);
      font-size: 0.75rem;
      padding: 6px 14px;
      border-radius: 6px;
      border: 1px solid var(--border);
      background: transparent;
      color: var(--text-dim);
      cursor: pointer;
      text-decoration: none;
      transition: all 0.2s;
    }}
    .doc-btn:hover {{
      border-color: var(--accent-dim);
      color: var(--accent);
    }}

    /* ── Layout ── */
    .doc-layout {{
      display: flex;
      flex: 1;
      overflow: hidden;
      position: relative;
      z-index: 1;
    }}

    /* ── Sidebar ── */
    .doc-sidebar {{
      width: 240px;
      flex-shrink: 0;
      border-right: 1px solid var(--border);
      background: var(--bg-card);
      overflow-y: auto;
      padding: 20px 0;
    }}

    .doc-sidebar::-webkit-scrollbar {{ width: 6px; }}
    .doc-sidebar::-webkit-scrollbar-track {{ background: transparent; }}
    .doc-sidebar::-webkit-scrollbar-thumb {{ background: var(--border); border-radius: 3px; }}
    .doc-sidebar::-webkit-scrollbar-thumb:hover {{ background: var(--text-dim); }}

    .sidebar-section {{
      margin-bottom: 20px;
    }}

    .sidebar-heading {{
      font-family: var(--mono);
      font-size: 0.7rem;
      text-transform: uppercase;
      letter-spacing: 0.12em;
      color: var(--text-dim);
      padding: 0 20px;
      margin-bottom: 6px;
    }}

    .sidebar-link {{
      display: flex;
      align-items: center;
      justify-content: space-between;
      padding: 5px 20px;
      font-size: 0.82rem;
      color: var(--text);
      text-decoration: none;
      transition: all 0.15s;
    }}
    .sidebar-link:hover {{
      color: var(--accent);
      background: rgba(79,195,247,0.04);
    }}
    .sidebar-link.active {{
      color: var(--accent);
      background: rgba(79,195,247,0.08);
      border-right: 2px solid var(--accent);
    }}

    .sidebar-count {{
      font-family: var(--mono);
      font-size: 0.65rem;
      color: var(--text-dim);
      background: rgba(255,255,255,0.04);
      padding: 1px 6px;
      border-radius: 8px;
    }}

    .sidebar-empty {{
      padding: 20px;
      font-size: 0.82rem;
      color: var(--text-dim);
      line-height: 1.5;
    }}

    /* ── Mobile sidebar toggle ── */
    .sidebar-toggle {{
      display: none;
      position: fixed;
      bottom: 20px;
      right: 20px;
      z-index: 20;
      width: 44px;
      height: 44px;
      border-radius: 50%;
      border: 1px solid var(--border);
      background: var(--bg-card);
      color: var(--accent);
      font-size: 1.2rem;
      cursor: pointer;
      align-items: center;
      justify-content: center;
      box-shadow: 0 4px 12px rgba(0,0,0,0.4);
    }}

    /* ── Content ── */
    .doc-content {{
      flex: 1;
      overflow-y: auto;
      padding: 24px 32px 60px;
    }}

    .doc-content::-webkit-scrollbar {{ width: 8px; }}
    .doc-content::-webkit-scrollbar-track {{ background: transparent; }}
    .doc-content::-webkit-scrollbar-thumb {{ background: var(--border); border-radius: 4px; }}
    .doc-content::-webkit-scrollbar-thumb:hover {{ background: var(--text-dim); }}

    /* ── Search ── */
    .doc-search-wrap {{
      position: sticky;
      top: 0;
      background: var(--bg);
      padding: 0 0 16px;
      z-index: 5;
      margin-bottom: 8px;
    }}

    #doc-search {{
      width: 100%;
      max-width: 480px;
      font-family: var(--mono);
      font-size: 0.85rem;
      padding: 10px 16px;
      border-radius: 8px;
      border: 1px solid var(--border);
      background: var(--bg-card);
      color: var(--text);
      outline: none;
      transition: border-color 0.2s;
    }}
    #doc-search:focus {{
      border-color: var(--accent-dim);
    }}
    #doc-search::placeholder {{
      color: var(--text-dim);
      opacity: 0.6;
    }}

    .doc-stats {{
      font-family: var(--mono);
      font-size: 0.72rem;
      color: var(--text-dim);
      margin-top: 8px;
    }}

    /* ── Section label & title ── */
    .section-label {{
      font-family: var(--mono);
      font-size: 0.75rem;
      text-transform: uppercase;
      letter-spacing: 0.15em;
      color: var(--accent);
      margin-bottom: 6px;
    }}

    .section-title {{
      font-size: 1.5rem;
      font-weight: 600;
      color: var(--heading);
      margin-bottom: 16px;
      padding-top: 8px;
    }}

    /* ── Doc category ── */
    .doc-category {{
      margin-bottom: 40px;
    }}

    /* ── Doc entry ── */
    .doc-entry {{
      background: var(--bg-card);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 20px;
      margin-bottom: 12px;
      transition: border-color 0.2s;
    }}
    .doc-entry:hover {{
      border-color: var(--accent-dim);
    }}
    .doc-entry.hidden {{
      display: none;
    }}

    .doc-sig {{
      font-family: var(--mono);
      font-size: 0.9rem;
      line-height: 1.5;
      margin-bottom: 8px;
    }}
    .doc-sig .fn {{
      color: var(--fn);
      font-weight: 600;
    }}
    .doc-sig .typ {{
      color: var(--type);
    }}
    .doc-sig .op {{
      color: var(--text-dim);
    }}
    .doc-sig .text {{
      color: var(--text);
    }}

    .doc-desc {{
      font-size: 0.875rem;
      color: var(--text-dim);
      line-height: 1.6;
      margin-bottom: 8px;
    }}

    .doc-examples {{
      margin-top: 10px;
    }}

    .doc-examples pre {{
      background: rgba(0,0,0,0.2);
      border: 1px solid var(--border);
      border-radius: 6px;
      padding: 10px 14px;
      margin-bottom: 6px;
      overflow-x: auto;
    }}

    .doc-examples code {{
      font-family: var(--mono);
      font-size: 0.82rem;
      color: var(--text);
      line-height: 1.6;
    }}

    .doc-examples .cmt {{
      color: var(--comment);
      font-style: italic;
    }}

    /* Syntax highlighting classes (match site) */
    .kw {{ color: var(--keyword); }}
    .str {{ color: var(--string); }}
    .cmt {{ color: var(--comment); font-style: italic; }}
    .typ {{ color: var(--type); }}
    .num {{ color: var(--number); }}
    .fn {{ color: var(--fn); }}
    .op {{ color: var(--text-dim); }}

    /* ── Empty state ── */
    .doc-empty {{
      text-align: center;
      padding: 80px 24px;
    }}

    .doc-empty-icon {{
      font-size: 3rem;
      color: var(--accent-dim);
      margin-bottom: 16px;
    }}

    .doc-empty h2 {{
      font-size: 1.5rem;
      font-weight: 600;
      color: var(--heading);
      margin-bottom: 12px;
    }}

    .doc-empty p {{
      color: var(--text-dim);
      margin-bottom: 24px;
      max-width: 520px;
      margin-left: auto;
      margin-right: auto;
    }}

    .doc-empty code {{
      font-family: var(--mono);
      font-size: 0.82rem;
      background: var(--bg-card);
      padding: 2px 6px;
      border-radius: 4px;
      border: 1px solid var(--border);
    }}

    .doc-empty-example {{
      max-width: 560px;
      margin: 0 auto;
      text-align: left;
    }}

    .doc-empty-example pre {{
      background: var(--bg-card);
      border: 1px solid var(--border);
      border-radius: 8px;
      padding: 16px 20px;
      overflow-x: auto;
      font-family: var(--mono);
      font-size: 0.82rem;
      line-height: 1.7;
    }}

    .doc-empty-example code {{
      font-family: var(--mono);
      font-size: 0.82rem;
      background: none;
      border: none;
      padding: 0;
    }}

    /* ── Footer ── */
    .doc-footer {{
      padding: 24px 32px;
      border-top: 1px solid var(--border);
      text-align: center;
      flex-shrink: 0;
    }}

    .doc-footer-text {{
      font-size: 0.75rem;
      color: var(--text-dim);
    }}

    /* ── Responsive ── */
    @media (max-width: 768px) {{
      .doc-sidebar {{
        position: fixed;
        left: -260px;
        top: 0;
        bottom: 0;
        z-index: 15;
        transition: left 0.25s ease;
        box-shadow: none;
      }}
      .doc-sidebar.open {{
        left: 0;
        box-shadow: 4px 0 20px rgba(0,0,0,0.5);
      }}

      .sidebar-toggle {{
        display: flex;
      }}

      .sidebar-overlay {{
        display: none;
        position: fixed;
        inset: 0;
        background: rgba(0,0,0,0.5);
        z-index: 14;
      }}
      .sidebar-overlay.open {{
        display: block;
      }}

      .doc-content {{
        padding: 20px 16px 60px;
      }}

      .doc-header {{
        padding: 10px 16px;
      }}

      .doc-title {{ display: none; }}
      .doc-sep {{ display: none; }}
    }}

    @media (max-width: 480px) {{
      .doc-entry {{
        padding: 14px;
      }}
      .doc-sig {{
        font-size: 0.8rem;
      }}
    }}
  </style>
</head>
<body>

<!-- Header -->
<div class="doc-header">
  <div class="doc-header-left">
    <a href="/" class="doc-logo">Lattice</a>
    <span class="doc-sep">/</span>
    <span class="doc-title">Documentation</span>
  </div>
  <div class="doc-header-right">
    <a href="playground.html" class="doc-btn">Playground</a>
    <a href="/" class="doc-btn">Home</a>
    <a href="https://github.com/ajokela/lattice" class="doc-btn">GitHub</a>
  </div>
</div>

<!-- Layout -->
<div class="doc-layout">

  <!-- Sidebar -->
  <nav class="doc-sidebar" id="doc-sidebar">
{sidebar_html}  </nav>

  <!-- Overlay for mobile sidebar -->
  <div class="sidebar-overlay" id="sidebar-overlay"></div>

  <!-- Content -->
  <main class="doc-content" id="doc-content">
    <div class="doc-search-wrap">
      <input type="text" id="doc-search" placeholder="Search functions..." autocomplete="off" spellcheck="false">
      <div class="doc-stats">{total_entries} functions across {total_categories} categories</div>
    </div>

{content_html}  </main>
</div>

<!-- Mobile sidebar toggle -->
<button class="sidebar-toggle" id="sidebar-toggle" aria-label="Toggle navigation">&#9776;</button>

<script>
(function() {{
  'use strict';

  // ── Search / Filter ──
  var searchInput = document.getElementById('doc-search');
  var entries = document.querySelectorAll('.doc-entry');
  var categories = document.querySelectorAll('.doc-category');

  searchInput.addEventListener('input', function() {{
    var q = this.value.toLowerCase().trim();

    for (var i = 0; i < entries.length; i++) {{
      var entry = entries[i];
      var name = entry.getAttribute('data-name') || '';
      var desc = entry.getAttribute('data-desc') || '';

      if (!q || name.indexOf(q) !== -1 || desc.indexOf(q) !== -1) {{
        entry.classList.remove('hidden');
      }} else {{
        entry.classList.add('hidden');
      }}
    }}

    // Hide categories with all entries hidden
    for (var j = 0; j < categories.length; j++) {{
      var cat = categories[j];
      var visibleEntries = cat.querySelectorAll('.doc-entry:not(.hidden)');
      cat.style.display = visibleEntries.length === 0 ? 'none' : '';
    }}
  }});

  // ── Sidebar active state on scroll ──
  var contentEl = document.getElementById('doc-content');
  var sidebarLinks = document.querySelectorAll('.sidebar-link');

  function updateActiveLink() {{
    var scrollTop = contentEl.scrollTop;
    var current = '';

    for (var i = 0; i < categories.length; i++) {{
      var cat = categories[i];
      if (cat.offsetTop - 120 <= scrollTop) {{
        current = cat.id;
      }}
    }}

    for (var j = 0; j < sidebarLinks.length; j++) {{
      var link = sidebarLinks[j];
      if (link.getAttribute('data-cat') === current) {{
        link.classList.add('active');
      }} else {{
        link.classList.remove('active');
      }}
    }}
  }}

  contentEl.addEventListener('scroll', updateActiveLink);
  updateActiveLink();

  // ── Sidebar link click: scroll content ──
  for (var k = 0; k < sidebarLinks.length; k++) {{
    sidebarLinks[k].addEventListener('click', function(e) {{
      e.preventDefault();
      var targetId = this.getAttribute('data-cat');
      var target = document.getElementById(targetId);
      if (target) {{
        contentEl.scrollTo({{ top: target.offsetTop - 16, behavior: 'smooth' }});
      }}
      // Close mobile sidebar
      document.getElementById('doc-sidebar').classList.remove('open');
      document.getElementById('sidebar-overlay').classList.remove('open');
    }});
  }}

  // ── Mobile sidebar toggle ──
  var toggleBtn = document.getElementById('sidebar-toggle');
  var sidebar = document.getElementById('doc-sidebar');
  var overlay = document.getElementById('sidebar-overlay');

  toggleBtn.addEventListener('click', function() {{
    sidebar.classList.toggle('open');
    overlay.classList.toggle('open');
  }});

  overlay.addEventListener('click', function() {{
    sidebar.classList.remove('open');
    overlay.classList.remove('open');
  }});

  // ── Keyboard shortcut: / to focus search ──
  document.addEventListener('keydown', function(e) {{
    if (e.key === '/' && document.activeElement !== searchInput) {{
      e.preventDefault();
      searchInput.focus();
    }}
    if (e.key === 'Escape' && document.activeElement === searchInput) {{
      searchInput.blur();
    }}
  }});
}})();
</script>

</body>
</html>
"""

    # Write output
    output_dir = Path(output_path).parent
    output_dir.mkdir(parents=True, exist_ok=True)

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(full_html)

    return total_entries, total_categories


# ── Main ────────────────────────────────────────────────────────────────────

def main():
    # Determine project root (script is in scripts/)
    script_dir = Path(__file__).resolve().parent
    project_root = script_dir.parent

    # Parse arguments
    src_dir = sys.argv[1] if len(sys.argv) > 1 else str(project_root / "src")
    output_file = sys.argv[2] if len(sys.argv) > 2 else str(project_root / "lattice-lang.org" / "docs.html")

    print(f"Scanning source files in: {src_dir}")
    entries = scan_source_files(src_dir)
    print(f"Found {len(entries)} documented entries")

    groups = group_by_category(entries)
    if groups:
        print(f"Categories: {', '.join(groups.keys())}")

    total_entries, total_categories = generate_html(groups, output_file)
    print(f"Generated: {output_file}")
    print(f"  {total_entries} functions across {total_categories} categories")


if __name__ == "__main__":
    main()
