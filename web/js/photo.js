// Client-side photo shrinking and upload.
import { ApiError } from './api.js';

// Scale so the longer side is <= max; keep aspect, round, never upscale.
export function fitSize(w, h, max = 1600) {
  const longer = Math.max(w, h);
  if (longer <= max) return { w, h };
  const s = max / longer;
  return { w: Math.max(1, Math.round(w * s)), h: Math.max(1, Math.round(h * s)) };
}

export async function resizeToJpeg(file) {
  const bmp = await createImageBitmap(file, { imageOrientation: 'from-image' });
  const { w, h } = fitSize(bmp.width, bmp.height);
  const canvas = document.createElement('canvas');
  canvas.width = w;
  canvas.height = h;
  canvas.getContext('2d').drawImage(bmp, 0, 0, w, h);
  bmp.close();
  return new Promise((resolve, reject) => {
    canvas.toBlob((blob) => (blob ? resolve(blob) : reject(new Error('Das Foto konnte nicht verarbeitet werden.'))),
      'image/jpeg', 0.85);
  });
}

export async function apiUpload(path, blob) {
  let res;
  try {
    res = await fetch(path, {
      method: 'POST', credentials: 'same-origin', headers: { 'Content-Type': 'image/jpeg' }, body: blob,
    });
  } catch {
    throw new ApiError(0, 'network', 'Keine Verbindung zum Server.');
  }
  let data = null;
  const text = await res.text();
  if (text) {
    try { data = JSON.parse(text); } catch { data = null; }
  }
  if (!res.ok) {
    throw new ApiError(res.status, (data && data.error) || 'error',
      (data && data.message) || 'Es ist ein Fehler aufgetreten.');
  }
  return data;
}
