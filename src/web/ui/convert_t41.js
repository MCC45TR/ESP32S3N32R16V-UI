const fs = require('fs');
const path = require('path');

const projectRoot = path.resolve(__dirname, '..', '..', '..');
const sourcePath = path.join(projectRoot, 'TEENSY41_CONNECTIONS.md');
const outputPath = path.join(__dirname, 'web_js_t41_html.txt');
const md = fs.readFileSync(sourcePath, 'utf8');

let html = md.replace(/^### (.*$)/gim, '<h4>$1</h4>');
html = html.replace(/^## (.*$)/gim, '<h3 style="color:#4db8ff;">$1</h3>');
html = html.replace(/^# (.*$)/gim, '<h2>$1</h2>');
html = html.replace(/\*\*(.*?)\*\*/g, '<strong>$1</strong>');
html = html.replace(/\*(.*?)\*/g, '<em>$1</em>');
html = html.replace(/```mermaid([\s\S]*?)```/g, '<pre class="mermaid">$1</pre>');
html = html.replace(/```c([\s\S]*?)```/g, '<pre style="background:#222;padding:10px;border-radius:5px;"><code>$1</code></pre>');
html = html.replace(/`(.*?)`/g, '<code style="background:#333;padding:2px 4px;border-radius:3px;color:#e83e8c;">$1</code>');
html = html.replace(/^\s*-\s+(.*)/gim, '<li>$1</li>');
html = html.replace(/(<li>.*?<\/li>\n?)+/g, (match) => '<ul>\n' + match + '</ul>\n');
html = html.replace(/^\> (.*$)/gim, '<blockquote style="border-left:4px solid #4db8ff;padding-left:10px;color:#aaa;">$1</blockquote>');

function parseTable(match) {
    const lines = match.trim().split('\n');
    let out = '<table class="doc-table">\n';
    let isHeader = true;
    for (let line of lines) {
        if (line.includes('---')) {
            isHeader = false;
            continue;
        }
        const cells = line.split('|').map(c => c.trim()).filter(c => c);
        if (cells.length === 0) continue;
        out += '<tr>\n';
        for (let cell of cells) {
            const tag = isHeader ? 'th' : 'td';
            out += `<${tag}>${cell}</${tag}>\n`;
        }
        out += '</tr>\n';
    }
    out += '</table>\n';
    return out;
}

html = html.replace(/^\|.*\|$(\n^\|.*\|(?:$|\n))*/gm, parseTable);

html = html.split('\n\n\n').join('\n\n');
html = html.replace(/\n\n(?!(<h|<ul|<ol|<table|<pre|<blockquote))/g, '</p>\n<p>');
html = '<p>' + html + '</p>';
html = html.replace('<p></p>', '');
html = html.replace('<p><ul>', '<ul>').replace('</ul></p>', '</ul>');
html = html.replace('<p><table', '<table').replace('</table></p>', '</table>');
html = html.replace('<p><h', '<h').replace('</h3></p>', '</h3>').replace('</h4></p>', '</h4>').replace('</h2></p>', '</h2>');

// Escape backticks in regexes so it doesn't break C++ raw strings
html = html.replace(/`/g, '\\`');

fs.writeFileSync(outputPath, html);
