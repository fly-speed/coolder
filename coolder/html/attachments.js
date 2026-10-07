'use strict';

const composerAttachments = { items: [], processing: false, revision: 0 };
function clearComposerAttachments() {
  composerAttachments.items = [];
  composerAttachments.revision++;
  renderComposerAttachments();
}
function renderComposerAttachments() {
  const container = $('prompt-attachments');
  container.replaceChildren();
  container.hidden = !composerAttachments.items.length;
  for (const item of composerAttachments.items) {
    const chip = node('div', undefined, 'attachment-chip');
    if (item.image) {
      const preview = node('img');
      preview.src = item.url;
      preview.alt = item.name;
      chip.append(preview);
    }
    const label = node('span', item.name);
    label.title = item.name + ' · ' + Math.ceil(item.size / 1024) + ' KiB';
    chip.append(label);
    const remove = button('×', () => {
      if (state.submitting) return;
      composerAttachments.items = composerAttachments.items.filter(entry => entry !== item);
      renderComposerAttachments();
    });
    remove.title = t('移除附件');
    remove.setAttribute('aria-label', t('移除附件') + ': ' + item.name);
    remove.disabled = !!state.submitting;
    chip.append(remove);
    container.append(chip);
  }
  $('attach-files').disabled = composerAttachments.processing || !!state.submitting;
  $('attach-files').setAttribute('aria-busy', String(composerAttachments.processing));
}
function fileDataURL(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(reader.result);
    reader.onerror = () => reject(new Error(t('无法读取附件')));
    reader.readAsDataURL(file);
  });
}
async function addComposerAttachments(files) {
  if (!state.project) throw new Error(t('请先选择项目'));
  if (state.submitting || composerAttachments.processing)
    throw new Error(t('附件正在处理，请稍后再试'));
  const revision = composerAttachments.revision;
  composerAttachments.processing = true;
  renderComposerAttachments();
  try {
    for (const file of files) {
      if (revision !== composerAttachments.revision) return;
      const items = composerAttachments.items;
      if (items.length >= 8) throw new Error(t('最多添加 8 个附件'));
      if (
        !file.size ||
        file.size > 8 * 1024 * 1024 ||
        items.reduce((n, i) => n + i.size, 0) + file.size > 16 * 1024 * 1024
      )
        throw new Error(t('单个附件需为 1 字节至 8 MiB，合计不超过 16 MiB'));
      let name = file.name || 'clipboard-' + Date.now() + '.png';
      if (
        new TextEncoder().encode(name).length > 180 ||
        /[\x00-\x1f\x7f/\\<>"&:]/.test(name) ||
        name === '.' ||
        name === '..'
      )
        throw new Error(t('附件名称含不支持的字符'));
      const extension = name.split('.').pop().toLowerCase();
      const image = ['png', 'jpg', 'jpeg', 'gif', 'webp'].includes(extension);
      if (file.type.startsWith('image/') && !image)
        throw new Error(t('图片支持 PNG、JPEG、GIF 和 WebP'));
      let textBytes = 0;
      if (!image) {
        if (file.size > 32 * 1024)
          throw new Error(t('文本附件合计不超过 32 KiB，不支持二进制文档'));
        const bytes = await file.arrayBuffer();
        let text;
        try {
          text = new TextDecoder('utf-8', { fatal: true }).decode(bytes);
        } catch {
          throw new Error(t('文本附件合计不超过 32 KiB，不支持二进制文档'));
        }
        if (text.includes('\0')) throw new Error(t('文本附件合计不超过 32 KiB，不支持二进制文档'));
        textBytes = file.size + new TextEncoder().encode(name).length + 64;
        if (items.reduce((n, i) => n + i.textBytes, 0) + textBytes > 32 * 1024)
          throw new Error(t('文本附件合计不超过 32 KiB，不支持二进制文档'));
      }
      const url = await fileDataURL(file);
      if (revision !== composerAttachments.revision) return;
      items.push({ name, size: file.size, image, textBytes, url });
      renderComposerAttachments();
    }
  } finally {
    composerAttachments.processing = false;
    renderComposerAttachments();
  }
}
async function prepareComposerAttachments() {
  if (composerAttachments.processing) throw new Error(t('附件正在处理，请稍后再试'));
  if (!composerAttachments.items.length) return { attachments: [], attachment_draft: '' };
  return api('/attachments', {
    files: composerAttachments.items.map(item => ({
      name: item.name,
      base64: item.url.split(',')[1]
    }))
  });
}
async function discardComposerDraft(draft) {
  if (draft) await api('/attachments/discard', { attachment_draft: draft }).catch(() => {});
}
$('attach-files').onclick = () => $('attachment-input').click();
$('attachment-input').onchange = task(async () => {
  const files = [...$('attachment-input').files];
  $('attachment-input').value = '';
  await addComposerAttachments(files);
});
$('prompt').addEventListener('paste', event => {
  const data = event.clipboardData;
  const files = [...(data?.files || [])];
  if (!files.length)
    for (const item of data?.items || []) {
      if (item.kind === 'file') {
        const file = item.getAsFile();
        if (file) files.push(file);
      }
    }
  if (!files.length) return;
  event.preventDefault();
  // A mixed clipboard payload may also contain useful text.
  const text = data.getData('text/plain');
  if (text)
    $('prompt').setRangeText(text, $('prompt').selectionStart, $('prompt').selectionEnd, 'end');
  addComposerAttachments(files).catch(notice);
});
$('prompt-form').addEventListener('dragover', event => {
  if ([...event.dataTransfer.types].includes('Files')) event.preventDefault();
});
$('prompt-form').addEventListener('drop', event => {
  if (!event.dataTransfer.files.length) return;
  event.preventDefault();
  addComposerAttachments([...event.dataTransfer.files]).catch(notice);
});
