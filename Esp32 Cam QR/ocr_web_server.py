"""
ESP32 QR Code Reader — Server
==============================
The ESP32-CAM uploads a JPEG to /upload.
The server decodes any QR code in the image using multiple strategies:
  1. pyzbar  (fastest, works on clean captures)
  2. OpenCV  QRCodeDetector (built-in fallback)
  3. Preprocessed versions of the image (contrast boost, perspective fix)
     retried through both decoders.

pip install flask opencv-contrib-python pyzbar pillow numpy
On Windows you also need the Visual C++ redistributable for pyzbar's zbar DLL.
"""

from flask import Flask, request, jsonify, send_file
import cv2
import numpy as np
import os
import base64
from datetime import datetime

# pyzbar is optional — graceful fallback to OpenCV-only if not installed
try:
    from pyzbar import pyzbar
    PYZBAR_AVAILABLE = True
except ImportError:
    PYZBAR_AVAILABLE = False
    print("[WARN] pyzbar not installed — using OpenCV decoder only.")
    print("       Install with:  pip install pyzbar")

app = Flask(__name__)

LATEST_IMAGE    = "latest.jpg"
PROCESSED_IMAGE = "processed.png"

latest_result = {
    "data":     "No QR yet",
    "type":     "-",
    "method":   "-",
    "time":     "-",
    "status":   "waiting",
}
capture_requested = False

# ─────────────────────────────────────────────────────────────────────────────
# HTML dashboard
# ─────────────────────────────────────────────────────────────────────────────
HTML = """<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <title>ESP32 QR Reader</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    * { box-sizing: border-box; }
    body { font-family: Arial, sans-serif; background: #f0f2f5; margin: 0; padding: 20px; color: #222; }
    .wrap { max-width: 900px; margin: auto; }
    .card { background: white; border-radius: 16px; padding: 20px; margin-bottom: 18px;
            box-shadow: 0 4px 16px rgba(0,0,0,0.08); }
    h2 { margin-top: 0; }

    /* QR result box */
    .result-box {
      font-size: 22px; font-weight: bold; text-align: center;
      padding: 20px 16px; border-radius: 12px;
      background: #f9fafb; border: 2px solid #e5e7eb;
      word-break: break-all; min-height: 72px;
      transition: border-color .3s, background .3s;
    }
    .result-box.success { border-color: #22c55e; background: #f0fdf4; color: #15803d; }
    .result-box.error   { border-color: #f87171; background: #fef2f2; color: #b91c1c; }

    /* URL opens as link */
    .result-box a { color: inherit; text-decoration: underline; }

    .meta  { font-size: 13px; color: #888; text-align: center; margin-top: 8px; }
    .badge { display: inline-block; padding: 2px 10px; border-radius: 20px;
             font-size: 12px; font-weight: bold; margin-left: 6px;
             background: #dbeafe; color: #1d4ed8; }
    .grid  { display: grid; grid-template-columns: 1fr 1fr; gap: 16px; }
    img    { width: 100%; border-radius: 10px; border: 1px solid #ddd; }
    h3     { margin: 0 0 10px; font-size: 14px; color: #555; }
    .btn-wrap { text-align: center; margin-top: 16px; }
    button { border: none; background: #111827; color: white; padding: 12px 32px;
             border-radius: 10px; cursor: pointer; font-size: 16px; }
    button:active { background: #374151; }
    .history-list { list-style: none; padding: 0; margin: 0; }
    .history-list li { padding: 8px 12px; border-bottom: 1px solid #f3f4f6;
                       font-size: 14px; display: flex; justify-content: space-between; }
    .history-list li:last-child { border-bottom: none; }
    .h-data { font-weight: bold; word-break: break-all; flex: 1; }
    .h-time { color: #aaa; font-size: 12px; white-space: nowrap; margin-left: 12px; }
    @media (max-width: 600px) { .grid { grid-template-columns: 1fr; } }
  </style>
</head>
<body>
<div class="wrap">

  <!-- Result card -->
  <div class="card">
    <h2>📷 ESP32 QR Code Reader</h2>
    <div class="result-box" id="resultBox">Waiting for QR code…</div>
    <div class="meta" id="metaBox">—</div>
    <div class="btn-wrap">
      <button onclick="captureNow()">Capture</button>
    </div>
  </div>

  <!-- Images -->
  <div class="grid">
    <div class="card"><h3>Latest Image</h3>       <img id="imgBox"  src="/image?t=0"></div>
    <div class="card"><h3>Processed Image</h3>    <img id="procBox" src="/processed?t=0"></div>
  </div>

  <!-- History -->
  <div class="card">
    <h2>History</h2>
    <ul class="history-list" id="historyList"><li style="color:#aaa">No scans yet</li></ul>
  </div>
</div>

<script>
const history = [];

function isURL(s) {
  try { new URL(s); return s.startsWith('http'); } catch { return false; }
}

function addHistory(data, time) {
  if (history.length && history[0].data === data) return; // deduplicate
  history.unshift({ data, time });
  if (history.length > 20) history.pop();
  const ul = document.getElementById('historyList');
  ul.innerHTML = history.map(h =>
    `<li><span class="h-data">${isURL(h.data)
      ? `<a href="${h.data}" target="_blank">${h.data}</a>`
      : escHtml(h.data)}</span>
    <span class="h-time">${h.time}</span></li>`
  ).join('');
}

function escHtml(s) {
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
}

async function captureNow() {
  try { await fetch('/capture', { method: 'POST' }); } catch(e) {}
}

async function poll() {
  try {
    const d = await (await fetch('/result')).json();
    const box = document.getElementById('resultBox');

    if (d.status === 'ok') {
      box.className = 'result-box success';
      box.innerHTML = isURL(d.data)
        ? `<a href="${d.data}" target="_blank">${d.data}</a>`
        : escHtml(d.data);
      addHistory(d.data, d.time);
    } else if (d.status === 'error') {
      box.className = 'result-box error';
      box.textContent = d.data;
    } else {
      box.className = 'result-box';
      box.textContent = d.data;
    }

    const badge = d.method !== '-'
      ? `<span class="badge">${d.method}</span>` : '';
    const typeStr = d.type !== '-' ? `  Type: ${d.type}` : '';
    document.getElementById('metaBox').innerHTML =
      `${d.time}${typeStr}${badge}`;

    const t = Date.now();
    document.getElementById('imgBox').src  = '/image?t='     + t;
    document.getElementById('procBox').src = '/processed?t=' + t;
  } catch(e) {}
}

setInterval(poll, 1200);
poll();
</script>
</body>
</html>"""


# ─────────────────────────────────────────────────────────────────────────────
# Image preprocessing helpers
# ─────────────────────────────────────────────────────────────────────────────

def centre_crop(img, pct=0.04):
    """Remove thin outer border (kills JPEG compression artefacts at edges)."""
    h, w = img.shape[:2]
    y1, y2 = int(h * pct), int(h * (1 - pct))
    x1, x2 = int(w * pct), int(w * (1 - pct))
    return img[y1:y2, x1:x2]


def sharpen(img):
    kernel = np.array([[ 0, -1,  0],
                       [-1,  5, -1],
                       [ 0, -1,  0]], dtype=np.float32)
    return cv2.filter2D(img, -1, kernel)


def clahe_grey(img):
    grey  = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    clahe = cv2.createCLAHE(clipLimit=3.0, tileGridSize=(8, 8))
    return clahe.apply(grey)


def adaptive_binary(grey):
    """Adaptive threshold → clean black-on-white binary."""
    blur = cv2.GaussianBlur(grey, (3, 3), 0)
    return cv2.adaptiveThreshold(
        blur, 255,
        cv2.ADAPTIVE_THRESH_GAUSSIAN_C,
        cv2.THRESH_BINARY,
        blockSize=11, C=2,
    )


def rescale_if_small(img, min_dim=400):
    """QR decoders work best when the code is at least 400 px wide/tall."""
    h, w = img.shape[:2]
    smallest = min(h, w)
    if smallest < min_dim:
        scale = min_dim / smallest
        img = cv2.resize(img, None, fx=scale, fy=scale,
                         interpolation=cv2.INTER_CUBIC)
    return img


def build_variants(img):
    """
    Return a list of (name, image) tuples representing different preprocessed
    versions of the input, ordered from fastest/simplest to most aggressive.
    """
    img    = centre_crop(img)
    img    = rescale_if_small(img)
    grey   = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    sharp  = sharpen(img)
    eq     = clahe_grey(img)
    binary = adaptive_binary(eq)

    return [
        ("original",  img),
        ("grey",      grey),
        ("sharpened", sharp),
        ("clahe",     cv2.cvtColor(eq, cv2.COLOR_GRAY2BGR)),
        ("binary",    binary),
        ("inverted",  cv2.bitwise_not(binary)),   # some QR printers invert
    ]


# ─────────────────────────────────────────────────────────────────────────────
# QR decode — tries pyzbar then OpenCV on each image variant
# Returns first successful (data_str, qr_type, method_name) or None
# ─────────────────────────────────────────────────────────────────────────────

_cv_detector = cv2.QRCodeDetector()

def _try_pyzbar(img):
    if not PYZBAR_AVAILABLE:
        return None
    # pyzbar needs an 8-bit image
    if len(img.shape) == 3:
        img8 = img
    else:
        img8 = img
    codes = pyzbar.decode(img8)
    if codes:
        c = codes[0]
        return c.data.decode("utf-8", errors="replace"), c.type
    return None


def _try_opencv(img):
    if len(img.shape) == 3:
        grey = cv2.cvtColor(img, cv2.COLOR_BGR2GRAY)
    else:
        grey = img
    data, pts, _ = _cv_detector.detectAndDecode(grey)
    if data:
        return data, "QR_CODE"
    return None


def decode_qr(img):
    """
    Try all variants × both decoders.
    Returns (data, qr_type, method) or (None, None, None).
    """
    variants = build_variants(img)

    for name, variant in variants:
        # pyzbar first (generally more robust)
        res = _try_pyzbar(variant)
        if res:
            return res[0], res[1], f"pyzbar/{name}"

        # OpenCV fallback
        res = _try_opencv(variant)
        if res:
            return res[0], res[1], f"opencv/{name}"

    return None, None, None


# ─────────────────────────────────────────────────────────────────────────────
# Flask routes
# ─────────────────────────────────────────────────────────────────────────────

@app.route("/")
def home():
    return HTML


@app.route("/capture", methods=["POST"])
def capture():
    global capture_requested
    capture_requested = True
    return "OK", 200


@app.route("/should_capture")
def should_capture():
    global capture_requested
    if capture_requested:
        capture_requested = False
        return "1", 200
    return "0", 200


@app.route("/upload", methods=["POST"])
def upload():
    global latest_result
    try:
        img_bytes = request.data
        if not img_bytes:
            latest_result = {"data": "No image received", "type": "-",
                             "method": "-", "time": _now(), "status": "error"}
            return "No image", 400

        npimg = np.frombuffer(img_bytes, np.uint8)
        img   = cv2.imdecode(npimg, cv2.IMREAD_COLOR)
        if img is None:
            latest_result = {"data": "Bad image data", "type": "-",
                             "method": "-", "time": _now(), "status": "error"}
            return "Bad image", 400

        cv2.imwrite(LATEST_IMAGE, img)

        # Save the best processed variant as debug image (CLAHE grey)
        debug = clahe_grey(centre_crop(img))
        cv2.imwrite(PROCESSED_IMAGE, debug)

        # Attempt decode
        data, qr_type, method = decode_qr(img)

        if data:
            latest_result = {
                "data":   data,
                "type":   qr_type or "QR_CODE",
                "method": method,
                "time":   _now(),
                "status": "ok",
            }
            print(f"[QR] {repr(data)}  type={qr_type}  method={method}")
            return data, 200
        else:
            latest_result = {
                "data":   "No QR code found",
                "type":   "-",
                "method": "-",
                "time":   _now(),
                "status": "no_qr",
            }
            print("[QR] No QR code detected in image")
            return "No QR code found", 200

    except Exception as e:
        latest_result = {"data": f"Error: {e}", "type": "-",
                         "method": "-", "time": _now(), "status": "error"}
        print(f"[ERR] {e}")
        return f"Server error: {e}", 500


@app.route("/result")
def result():
    return jsonify(latest_result)


@app.route("/image")
def image():
    return send_file(LATEST_IMAGE,    mimetype="image/jpeg") \
        if os.path.exists(LATEST_IMAGE)    else ("No image yet",     404)


@app.route("/processed")
def processed():
    return send_file(PROCESSED_IMAGE, mimetype="image/png") \
        if os.path.exists(PROCESSED_IMAGE) else ("No processed image", 404)


def _now():
    return datetime.now().strftime("%Y-%m-%d %H:%M:%S")


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=False)