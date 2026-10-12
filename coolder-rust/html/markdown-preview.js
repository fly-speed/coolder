// Adapted from aicool/webcool/html/js/markdown-preview.js.
(function () {
  function mdEscapeHtml(value) {
    return String(value == null ? '' : value)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#39;');
  }

  function isMarkdownName(name) {
    return /\.(md|markdown|mdown|mkdn)$/i.test(String(name || ''));
  }

  function isSafeUrl(url) {
    const text = String(url || '').trim();
    return !/[\u0000-\u001f\u007f]/.test(text) &&
      (!/^[a-z][a-z0-9+.-]*:/i.test(text) || /^(https?:|mailto:|tel:)/i.test(text));
  }

  function escapeUrl(url) {
    const text = String(url || '').trim();
    return isSafeUrl(text) ? mdEscapeHtml(text) : '#';
  }

  function slugify(text, used) {
    let slug = String(text || '')
      .toLowerCase()
      .replace(/<[^>]*>/g, '')
      .replace(/&[a-z0-9#]+;/gi, '')
      .replace(/[^\w\u4e00-\u9fa5\s-]/g, '')
      .replace(/\s+/g, '-')
      .replace(/-+/g, '-')
      .replace(/^-+|-+$/g, '');
    if (!slug) {
      slug = 'section';
    }
    const count = used[slug] || 0;
    used[slug] = count + 1;
    return count ? slug + '-' + count : slug;
  }

  function inlineMarkdown(text) {
    const tokens = [];
    function store(html) {
      const token = '\u0000INLINE' + tokens.length + '\u0000';
      tokens.push(html);
      return token;
    }
    let value = String(text || '').replace(/\u0000/g, '');
    value = value.replace(/`([^`]+)`/g, function (_, body) {
      return store('<code>' + mdEscapeHtml(body) + '</code>');
    });
    value = value.replace(/<\/?[A-Za-z][^>]*>/g, function (tag) {
      return allowedHTMLTags.has(htmlTagName(tag)) ? store(sanitizeRawHTML(tag)) : tag;
    });
    value = value.replace(/!\[([^\]]*)\]\(([^)\s]+)(?:\s+"([^"]*)")?\)/g, function (_, alt, url, title) {
      return store('<img src="' + escapeUrl(url) + '" alt="' + mdEscapeHtml(alt) + '"' + (title ? ' title="' + mdEscapeHtml(title) + '"' : '') + '>');
    });
    value = value.replace(/\[([^\]]+)\]\(([^)\s]+)(?:\s+"([^"]*)")?\)/g, function (_, label, url, title) {
      const externalAttrs = url.charAt(0) === '#' ? '' : ' target="_blank" rel="noopener noreferrer"';
      return store('<a href="' + escapeUrl(url) + '"' + externalAttrs +
        (title ? ' title="' + mdEscapeHtml(title) + '"' : '') + '>' + mdEscapeHtml(label).replace(/(\*\*|__)(.+?)\1/g, '<strong>$2</strong>').replace(/(\*|_)([^*_]+?)\1/g, '<em>$2</em>') + '</a>');
    });
    value = mdEscapeHtml(value);
    value = value.replace(/(\*\*|__)(.+?)\1/g, '<strong>$2</strong>');
    value = value.replace(/(\*|_)([^*_]+?)\1/g, '<em>$2</em>');
    value = value.replace(/~~(.+?)~~/g, '<del>$1</del>');
    for (let pass = 0; pass <= tokens.length && /\u0000INLINE\d+\u0000/.test(value); pass += 1) {
      value = value.replace(/\u0000INLINE(\d+)\u0000/g, function (_, index) {
        return tokens[Number(index)] || '';
      });
    }
    return value;
  }

  function parseTable(lines, index) {
    if (index + 1 >= lines.length) {
      return null;
    }
    const head = lines[index];
    const divider = lines[index + 1];
    if (head.indexOf('|') < 0 || !/^\s*\|?\s*:?-{3,}:?\s*(\|\s*:?-{3,}:?\s*)+\|?\s*$/.test(divider)) {
      return null;
    }
    function cells(line) {
      return String(line || '').trim().replace(/^\|/, '').replace(/\|$/, '').split('|').map(function (cell) {
        return cell.trim();
      });
    }
    const headers = cells(head);
    const aligns = cells(divider).map(function (cell) {
      const left = /^:/.test(cell);
      const right = /:$/.test(cell);
      return left && right ? 'center' : (right ? 'right' : (left ? 'left' : ''));
    });
    let rowIndex = index + 2;
    const rows = [];
    while (rowIndex < lines.length && lines[rowIndex].indexOf('|') >= 0 && String(lines[rowIndex]).trim()) {
      rows.push(cells(lines[rowIndex]));
      rowIndex += 1;
    }
    let html = '<table><thead><tr>';
    headers.forEach(function (cell, cellIndex) {
      html += '<th' + (aligns[cellIndex] ? ' style="text-align:' + aligns[cellIndex] + '"' : '') + '>' + inlineMarkdown(cell) + '</th>';
    });
    html += '</tr></thead>';
    if (rows.length) {
      html += '<tbody>';
      rows.forEach(function (row) {
        html += '<tr>';
        headers.forEach(function (_, cellIndex) {
          html += '<td' + (aligns[cellIndex] ? ' style="text-align:' + aligns[cellIndex] + '"' : '') + '>' + inlineMarkdown(row[cellIndex] || '') + '</td>';
        });
        html += '</tr>';
      });
      html += '</tbody>';
    }
    html += '</table>';
    return { html: html, nextIndex: rowIndex };
  }

  const allowedHTMLTags = new Set([
    'a', 'b', 'i', 's', 'u', 'hr', 'br', 'caption', 'code', 'del', 'details', 'div', 'em', 'img', 'kbd',
    'li', 'ol', 'p', 'pre', 'span', 'strong', 'sub', 'summary', 'sup', 'table',
    'tbody', 'td', 'tfoot', 'th', 'thead', 'tr', 'ul'
  ]);

  const htmlBlockTags = new Set([
    'div', 'table', 'thead', 'tbody', 'tfoot', 'tr', 'td', 'th', 'details', 'summary'
  ]);

  function htmlTagName(line) {
    const match = String(line || '').trim().match(/^<\/?\s*([A-Za-z][A-Za-z0-9-]*)\b/);
    return match ? match[1].toLowerCase() : '';
  }

  function isHTMLBlockStart(line) {
    const tag = htmlTagName(line);
    return tag && allowedHTMLTags.has(tag);
  }

  function matchesClosingTag(line, tag) {
    return new RegExp('<\\/\\s*' + tag.replace(/[.*+?^${}()|[\]\\]/g, '\\$&') + '\\s*>', 'i')
      .test(String(line || ''));
  }

  // Render embedded HTML as text. Markdown previews never execute document code.
  function sanitizeRawHTML(html) { return mdEscapeHtml(html); }

  function parseHTMLBlock(lines, index) {
    if (index >= lines.length || !isHTMLBlockStart(lines[index])) {
      return null;
    }
    const rootTag = htmlTagName(lines[index]);
    const body = [];
    let rowIndex = index;

    if (rootTag && htmlBlockTags.has(rootTag)) {
      while (rowIndex < lines.length) {
        const line = lines[rowIndex];
        body.push(line);
        rowIndex += 1;
        if (matchesClosingTag(line, rootTag)) {
          break;
        }
      }
    } else {
      while (rowIndex < lines.length) {
        const line = lines[rowIndex];
        if (!isHTMLBlockStart(line) && String(line || '').trim()) {
          break;
        }
        body.push(line);
        rowIndex += 1;
        if (!String(line || '').trim()) {
          break;
        }
      }
    }

    return { html: sanitizeRawHTML(body.join('\n')), nextIndex: rowIndex };
  }

  function renderMarkdown(markdown) {
    const lines = String(markdown == null ? '' : markdown).replace(/\r\n?/g, '\n').split('\n');
    const usedSlugs = Object.create(null);
    const html = [];
    let i = 0;

    function collectParagraph() {
      const parts = [];
      const start = i;
      while (i < lines.length && String(lines[i]).trim()) {
        if (/^\s*(```|~~~)/.test(lines[i]) || /^\s{0,3}(#{1,6})\s+/.test(lines[i]) || /^\s*[-*_]{3,}\s*$/.test(lines[i]) || /^\s*>/.test(lines[i]) || isHTMLBlockStart(lines[i]) || /^\s*([-+*]|\d+\.)\s+/.test(lines[i])) {
          break;
        }
        parts.push(lines[i]);
        i += 1;
      }
      if (parts.length) {
        html.push('<p>' + inlineMarkdown(parts.join(' ')) + '</p>');
      } else if (i === start && i < lines.length) {
        i += 1;
      }
    }

    while (i < lines.length) {
      const line = lines[i];
      const trimmed = String(line || '').trim();
      if (!trimmed) {
        i += 1;
        continue;
      }

      const fence = line.match(/^\s*(```|~~~)\s*(.*?)\s*$/);
      if (fence) {
        const marker = fence[1];
        const lang = fence[2] || '';
        i += 1;
        const body = [];
        while (i < lines.length && !new RegExp('^\\s*' + marker + '\\s*$').test(lines[i])) {
          body.push(lines[i]);
          i += 1;
        }
        if (i < lines.length) {
          i += 1;
        }
        html.push('<pre><code' + (lang ? ' class="language-' + mdEscapeHtml(lang) + '"' : '') + '>' + mdEscapeHtml(body.join('\n')) + '</code></pre>');
        continue;
      }

      const heading = line.match(/^\s{0,3}(#{1,6})\s+(.+?)\s*#*\s*$/);
      if (heading) {
        const level = heading[1].length;
        const body = inlineMarkdown(heading[2]);
        html.push('<h' + level + ' id="' + mdEscapeHtml(slugify(heading[2], usedSlugs)) + '">' + body + '</h' + level + '>');
        i += 1;
        continue;
      }

      if (/^\s*[-*_]{3,}\s*$/.test(line)) {
        html.push('<hr>');
        i += 1;
        continue;
      }

      if (/^\s*>/.test(line)) {
        const parts = [];
        while (i < lines.length && /^\s*>/.test(lines[i])) {
          parts.push(lines[i].replace(/^\s*>\s?/, ''));
          i += 1;
        }
        html.push('<blockquote>' + renderMarkdown(parts.join('\n')) + '</blockquote>');
        continue;
      }

      const htmlBlock = parseHTMLBlock(lines, i);
      if (htmlBlock) {
        html.push(htmlBlock.html);
        i = htmlBlock.nextIndex;
        continue;
      }

      const table = parseTable(lines, i);
      if (table) {
        html.push(table.html);
        i = table.nextIndex;
        continue;
      }

      const list = line.match(/^(\s*)([-+*]|\d+\.)\s+(.*)$/);
      if (list) {
        const ordered = /\d+\./.test(list[2]);
        const tag = ordered ? 'ol' : 'ul';
        html.push('<' + tag + '>');
        while (i < lines.length) {
          const item = lines[i].match(/^(\s*)([-+*]|\d+\.)\s+(.*)$/);
          if (!item || (/\d+\./.test(item[2]) !== ordered)) {
            break;
          }
          const task = item[3].match(/^\[([ xX])\]\s+(.*)$/);
          if (task) {
            html.push('<li class="task-list-item"><input type="checkbox" disabled' + (task[1].toLowerCase() === 'x' ? ' checked' : '') + '> ' + inlineMarkdown(task[2]) + '</li>');
          } else {
            html.push('<li>' + inlineMarkdown(item[3]) + '</li>');
          }
          i += 1;
        }
        html.push('</' + tag + '>');
        continue;
      }

      collectParagraph();
    }

    return html.join('\n') || '<p class="markdown-empty">空文档</p>';
  }

  window.CoolderMarkdown = { isMarkdownName, render: renderMarkdown };
}());
