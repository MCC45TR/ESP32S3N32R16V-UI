import re
from pathlib import Path

project_root = Path(__file__).resolve().parents[3]
source_path = project_root / 'TEENSY41_CONNECTIONS.md'
output_path = Path(__file__).with_name('web_js_t41_html.txt')

with source_path.open('r', encoding='utf-8') as f:
    md = f.read()

# Replace Headers
html = re.sub(r'(?m)^### (.*$)', r'<h4>\1</h4>', md)
html = re.sub(r'(?m)^## (.*$)', r'<h3 style="color:#4db8ff;">\1</h3>', html)
html = re.sub(r'(?m)^# (.*$)', r'<h2>\1</h2>', html)

# Replace Formatting
html = re.sub(r'\*\*(.*?)\*\*', r'<strong>\1</strong>', html)
html = re.sub(r'\*(.*?)\*', r'<em>\1</em>', html)
html = re.sub(r'```mermaid([\s\S]*?)```', r'<pre class="mermaid">\1</pre>', html)
html = re.sub(r'```c([\s\S]*?)```', r'<pre style="background:#222;padding:10px;border-radius:5px;"><code>\1</code></pre>', html)
html = re.sub(r'`(.*?)`', r'<code style="background:#333;padding:2px 4px;border-radius:3px;color:#e83e8c;">\1</code>', html)

# Replace Lists
html = re.sub(r'(?m)^\s*-\s+(.*)', r'<li>\1</li>', html)

# A simple hack for ul clustering
html = re.sub(r'(<li>.*?</li>\n?)+', lambda m: '<ul>\n' + m.group(0) + '</ul>\n', html)

# Replace Blockquotes
html = re.sub(r'(?m)^\> (.*$)', r'<blockquote style="border-left:4px solid #4db8ff;padding-left:10px;color:#aaa;">\1</blockquote>', html)


# Replace Tables
def parse_table(match):
    lines = match.group(0).strip().split('\n')
    out = '<table class="doc-table">\n'
    is_header = True
    for line in lines:
        if '---' in line:
            is_header = False
            continue
        cells = [c.strip() for c in line.split('|') if c.strip()]
        if not cells: continue
        out += '<tr>\n'
        for cell in cells:
            tag = 'th' if is_header else 'td'
            out += f'<{tag}>{cell}</{tag}>\n'
        out += '</tr>\n'
    out += '</table>\n'
    return out

html = re.sub(r'(?m)^\|.*\|$(\n^\|.*\|(?:$|\n))*', parse_table, html)

# Replace Paragraphs (double newline to p tags)
html = html.replace('\n\n\n', '\n\n')
html = re.sub(r'\n\n(?!(<h|<ul|<ol|<table|<pre|<blockquote))', r'</p>\n<p>', html)

html = '<p>' + html + '</p>'
html = html.replace('<p></p>', '')
html = html.replace('<p><ul>', '<ul>').replace('</ul></p>', '</ul>')
html = html.replace('<p><table', '<table').replace('</table></p>', '</table>')
html = html.replace('<p><h', '<h').replace('</h3></p>', '</h3>').replace('</h4></p>', '</h4>').replace('</h2></p>', '</h2>')

with output_path.open('w', encoding='utf-8') as f:
    f.write(html)
