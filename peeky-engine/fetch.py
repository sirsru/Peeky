import os
import re
import time
import html as htmllib
import base64
import hashlib
import pycurl
from urllib.parse import urljoin, unquote_to_bytes

FIREFOX_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0"

CACHE_DIR = os.path.join(os.path.expanduser("~"), ".cache", "peeky")
MAX_RESOURCE = 8 * 1024 * 1024
MAX_IMAGES = 60
MAX_CSS_DEPTH = 5

IMG_EXTS = {"png", "jpg", "jpeg", "gif", "bmp", "webp", "tga", "psd", "hdr", "pnm", "ppm", "pgm"}
CT_EXT = {"image/png": "png", "image/jpeg": "jpg", "image/jpg": "jpg", "image/gif": "gif",
          "image/bmp": "bmp", "image/webp": "webp"}


#Wowie do I love tab completion!11

#low level stuff

def _read_local(url):
    path = url[7:]
    if os.path.exists(path):
        with open(path, "rb") as f:
            return f.read()
    return b""


def _request(url, accept, ua=FIREFOX_UA, referer=None, timeout=30, limit=None):
    """GET with redirects, cookies, and gzip/deflate/br decoding.
    Returns (body, final_url, content_type, status)."""
    c = pycurl.Curl()
    out, headers = [], {}
    size = [0]

    def write(chunk):
        size[0] += len(chunk)
        if limit and size[0] > limit:
            return 0 #If too big, screw you
        out.append(chunk)
        return len(chunk)

    def header(line):
        line = line.decode("iso-8859-1")
        if ":" in line:
            k, v = line.split(":", 1)
            headers[k.strip().lower()] = v.strip()
        elif line.startswith("HTTP/"):
            headers.clear()
            
    hdrs = ["Accept: " + accept,
            "Accept-Language: en-US,en;q=0.9",
            "Upgrade-Insecure-Requests: 1"]
    if referer:
        hdrs.append("Referer: " + referer)
    try:
        c.setopt(pycurl.URL, url)
        c.setopt(pycurl.USERAGENT, ua)
        c.setopt(pycurl.HTTPHEADER, hdrs)
        c.setopt(pycurl.ACCEPT_ENCODING, "")      # all supported, auto-decompressed
        c.setopt(pycurl.FOLLOWLOCATION, 1)
        c.setopt(pycurl.MAXREDIRS, 10)
        c.setopt(pycurl.AUTOREFERER, 1)
        c.setopt(pycurl.COOKIEFILE, "")           # in-memory cookie jar (needed by redirect chains)
        c.setopt(pycurl.TIMEOUT, timeout)
        c.setopt(pycurl.CONNECTTIMEOUT, 10)
        c.setopt(pycurl.WRITEFUNCTION, write)
        c.setopt(pycurl.HEADERFUNCTION, header)
        c.setopt(pycurl.HTTP_VERSION, pycurl.CURL_HTTP_VERSION_2TLS)
        c.setopt(pycurl.TCP_KEEPALIVE, 1)
        c.setopt(pycurl.FORBID_REUSE, 0)
        c.perform()
        status = c.getinfo(pycurl.RESPONSE_CODE)
        final = c.getinfo(pycurl.EFFECTIVE_URL)
    finally:
        c.close()
    return b"".join(out), final, headers.get("content-type", ""), status


def fetch_html(url):
    if url.startswith("file://"):
        return _read_local(url)
    return _fetch_page_raw(url)[0]


def _fetch_page_raw(url):
    accept = "text/html, text/plain;q=0.9, */*;q=0.5"
    body, final, ctype, status = b"", url, "", 0
    for ua in (FIREFOX_UA):
        try:
            body, final, ctype, status = _request(url, accept, ua)
        except pycurl.error:
            if ua is FIREFOX_UA:
                raise
            continue
        if status < 400 and body.strip():
            break
    return body, final, ctype


def fetch_text(url):
    if url.startswith("file://"):
        return _read_local(url)
    return _request(url, "text/plain, */*;q=0.5")[0]


def fetch_image(url):
    if url.startswith("file://"):
        return _read_local(url)
    return _request(url, "image/webp,image/apng,image/*,*/*;q=0.8")[0]


# ---------------------------------------------------------------- cache

def _cache_path(key, ext):
    os.makedirs(CACHE_DIR, exist_ok=True)
    return os.path.join(CACHE_DIR, hashlib.sha1(key.encode("utf-8", "replace")).hexdigest() + "." + ext)


def _is_svg(data, ctype="", url=""):
    head = data[:512].lstrip().lower()
    return ("svg" in ctype.lower() or url.lower().split("?")[0].endswith((".svg", ".svgz"))
            or head.startswith(b"<svg") or (head.startswith(b"<?xml") and b"<svg" in data[:2048].lower()))


def _ext_for(url, ctype):
    ct = ctype.split(";")[0].strip().lower()
    if ct in CT_EXT:
        return CT_EXT[ct]
    ext = os.path.splitext(url.split("?")[0].split("#")[0])[1].lstrip(".").lower()
    return ext if ext in IMG_EXTS else "img"


def _decode_data_uri(uri):
    """data:[<mime>][;base64],<payload> -> (bytes, mime) or None."""
    try:
        meta, payload = uri[5:].split(",", 1)
    except ValueError:
        return None
    mime = meta.split(";")[0].strip().lower()
    try:
        if ";base64" in meta.lower():
            payload = unquote_to_bytes(payload)
            payload += b"=" * (-len(payload) % 4)
            return base64.b64decode(payload), mime
        return unquote_to_bytes(payload), mime
    except Exception:
        return None


class _Page:
    """Per-navigation state: base URL, cache bookkeeping, resource counters."""

    def __init__(self, base_url, referer):
        self.base = base_url
        self.referer = referer
        self.images = {}          # absolute url / data-uri key -> local path or None
        self.css_seen = set()

    def image(self, ref, base):
        """Resolve ref against base, download into cache, return local path or None."""
        ref = htmllib.unescape(ref.strip())
        if not ref:
            return None
        if ref.startswith("data:"):
            key = ref
        else:
            key = urljoin(base, ref)
        if key in self.images:
            return self.images[key]
        path = None
        if len(self.images) < MAX_IMAGES:
            try:
                path = self._load_image(key)
            except Exception:
                path = None
        self.images[key] = path
        return path

    def _load_image(self, key):
        if key.startswith("data:"):
            got = _decode_data_uri(key)
            if not got:
                return None
            data, mime = got
            ctype, url = mime, ""
        elif key.startswith(("http://", "https://")):
            data, _final, ctype, status = _request(
                key, "image/webp,image/apng,image/*,*/*;q=0.8",
                FIREFOX_UA, self.referer, timeout=15, limit=MAX_RESOURCE)
            if status >= 400:
                return None
            url = key
        else:
            return None
        if not data or _is_svg(data, ctype, url):
            return None               # stb_image can't decode SVG
        path = _cache_path(key, _ext_for(url, ctype))
        if not os.path.exists(path):
            with open(path, "wb") as f:
                f.write(data)
        return path

    # ---- CSS

    def _fetch_css(self, url, depth=0):
        #Fetch stylesheet and inline @imports
        if depth > MAX_CSS_DEPTH or url in self.css_seen:
            return ""
        self.css_seen.add(url)
        try:
            data, final, _ct, status = _request(
                url, "text/css,*/*;q=0.1", FIREFOX_UA, self.referer,
                timeout=15, limit=MAX_RESOURCE)
        except pycurl.error:
            return ""
        if status >= 400 or not data:
            return ""
        text = data.decode("utf-8", "replace").lstrip("\ufeff")

        def do_import(m):
            ref = m.group(1) or m.group(2) or m.group(3)
            media = (m.group(4) or "").strip().lower()
            if media and "print" in media and "screen" not in media and "all" not in media:
                return ""
            if ref.startswith("data:"):
                got = _decode_data_uri(ref)
                return got[0].decode("utf-8", "replace") if got else ""
            return self._fetch_css(urljoin(final, htmllib.unescape(ref)), depth + 1)

        return IMPORT_RE.sub(do_import, text)

    def css(self, url, depth=0):
        """Fetch a stylesheet, inline @imports, rewrite url() to cached files."""
        if depth > MAX_CSS_DEPTH or url in self.css_seen:
            return ""
        self.css_seen.add(url)
        try:
            data, final, _ct, status = _request(
                url, "text/css,*/*;q=0.1", FIREFOX_UA, self.referer,
                timeout=15, limit=MAX_RESOURCE)
        except pycurl.error:
            return ""
        if status >= 400 or not data:
            return ""
        text = data.decode("utf-8", "replace").lstrip("\ufeff")
        return self.process_css(text, final, depth)

    def process_css(self, text, base, depth=0):
        def do_import(m):
            ref = m.group(1) or m.group(2) or m.group(3)
            media = (m.group(4) or "").strip().lower()
            if media and "print" in media and "screen" not in media and "all" not in media:
                return ""
            if ref.startswith("data:"):
                got = _decode_data_uri(ref)
                return got[0].decode("utf-8", "replace") if got else ""
            return self.css(urljoin(base, htmllib.unescape(ref)), depth + 1)

        text = IMPORT_RE.sub(do_import, text)

        def do_url(m):
            ref = m.group(2).strip()
            if ref.startswith("#"):
                return m.group(0)
            ext = os.path.splitext(ref.split("?")[0].split("#")[0])[1].lower()
            if ext in (".woff", ".woff2", ".ttf", ".otf", ".eot", ".svg", ".cur", ".ico"):
                return m.group(0)
            if ref.startswith("data:font") or ref.startswith("data:application/font") \
                    or ref.startswith("data:image/svg"):
                return m.group(0)
            local = self.image(ref, base)
            return 'url("%s")' % local if local else m.group(0)

        return CSS_URL_RE.sub(do_url, text)


CSS_URL_RE = re.compile(r"""url\(\s*(["']?)(.*?)\1\s*\)""", re.I | re.S)
ATTR_RE = re.compile(r"""([\w:-]+)\s*=\s*("[^"]*"|'[^']*'|[^\s>]+)""")
LINK_RE = re.compile(r"<link\b[^>]*>", re.I)
IMG_RE = re.compile(r"<img\b[^>]*>", re.I)
STYLE_RE = re.compile(r"(<style\b[^>]*>)(.*?)(</style>)", re.I | re.S)
BASE_RE = re.compile(r"<base\b[^>]*>", re.I)
META_CHARSET_RE = re.compile(rb"""<meta[^>]+charset\s*=\s*["']?([\w-]+)""", re.I)

# @import groups: 1=url("...") 2=url(...) bare, 3="..." string form, 4=media
IMPORT_RE = re.compile(
    r"""@import\s+(?:url\(\s*(?:["']([^"']+)["']|([^)\s"']+))\s*\)|["']([^"']+)["'])\s*([^;]*);""",
    re.I)


def _attrs(tag):
    return {k.lower(): v.strip("\"'") for k, v in ATTR_RE.findall(tag)}


def _set_attr(tag, name, value):
    pat = re.compile(r"""(\b%s\s*=\s*)("[^"]*"|'[^']*'|[^\s>]+)""" % re.escape(name), re.I)
    new = '%s="%s"' % (name, value.replace('"', "%22"))
    if pat.search(tag):
        return pat.sub(lambda m: new, tag, count=1)
    return tag[:4] + " " + new + tag[4:]


def _decode_html(body, ctype):
    m = re.search(r"charset\s*=\s*([\w-]+)", ctype, re.I) or META_CHARSET_RE.search(body[:4096])
    if m:
        enc = m.group(1)
        if not isinstance(enc, str):
            enc = enc.decode("ascii", "ignore")
    else:
        enc = None
    for e in (enc, "utf-8"):
        if not e:
            continue
        try:
            return body.decode(e)
        except (LookupError, UnicodeDecodeError):
            continue
    return body.decode("utf-8", "replace")


# ---------------------------------------------------------------- public

def _rewrite_css_urls(text, page, base):
    #cache that shi
    def do_url(m):
        ref = m.group(2).strip()
        if ref.startswith("#"):
            return m.group(0)
        ext = os.path.splitext(ref.split("?")[0].split("#")[0])[1].lower()
        if ext in (".woff", ".woff2", ".ttf", ".otf", ".eot", ".svg", ".cur", ".ico"):
            return m.group(0)
        if ref.startswith("data:font") or ref.startswith("data:application/font") \
                or ref.startswith("data:image/svg"):
            return m.group(0)
        local = page.image(ref, base)
        return 'url("%s")' % local if local else m.group(0)
    return CSS_URL_RE.sub(do_url, text)


def fetch_page(url, on_status=None):
    if url.startswith("file://"):
        return _read_local(url)

    def mark(name, started):
        if on_status:
            on_status(name, time.monotonic() - started)

    started = time.monotonic()
    if on_status:
        on_status("fetch", None)
    body, final, ctype = _fetch_page_raw(url)
    mark("fetch", started)
    if not body:
        return body
    text = _decode_html(body, ctype)

    base = final
    bm = BASE_RE.search(text)
    if bm:
        href = _attrs(bm.group(0)).get("href")
        if href:
            base = urljoin(final, htmllib.unescape(href))

    page = _Page(base, final)

    #discover resource stuff ----------------------------------------------------------
    css_links = [] # absolute url of external stylesheets
    inline_styles = [] # css_text, css_base
    img_entries = [] # tag, ref, base

    def do_link_collect(m):
        a = _attrs(m.group(0))
        if "stylesheet" not in a.get("rel", "").lower() or not a.get("href"):
            return m.group(0)
        media = a.get("media", "").lower()
        if media and "print" in media and "screen" not in media and "all" not in media:
            return m.group(0)
        href = htmllib.unescape(a["href"])
        if href.startswith("data:"):
            got = _decode_data_uri(href)
            if got:
                inline_styles.append((got[0].decode("utf-8", "replace"), base))
            return m.group(0)
        css_links.append(urljoin(base, href))
        return m.group(0)

    text = LINK_RE.sub(do_link_collect, text)

    def do_style_collect(m):
        inline_styles.append((m.group(2), base))
        return m.group(0)
    text = STYLE_RE.sub(do_style_collect, text)

    def do_img_collect(m):
        tag = m.group(0)
        a = _attrs(tag)
        cands = []
        for k in ("src", "data-src", "data-original", "data-lazy-src"):
            if a.get(k):
                cands.append(a[k])
        if a.get("srcset") or a.get("data-srcset"):
            first = (a.get("srcset") or a.get("data-srcset")).split(",")[0].strip().split()
            if first:
                cands.append(first[0])
        for ref in cands:
            img_entries.append((tag, ref, base))
        return tag

    text = IMG_RE.sub(do_img_collect, text)

    #fetch all external stylesheets concurrently ------------------------------------
    from concurrent.futures import ThreadPoolExecutor, as_completed

    css_texts = {}
    if css_links:
        with ThreadPoolExecutor(max_workers=8) as ex:
            futs = {ex.submit(page._fetch_css, u): u for u in css_links}
            for f in as_completed(futs):
                css_texts[futs[f]] = f.result()
    mark("css", started)

    #find all image refs inside all CSS ---------------------------------------------
    for u, ct in css_texts.items():
        for m in CSS_URL_RE.finditer(ct):
            ref = m.group(2).strip()
            if ref.startswith("#") or ref.startswith("data:font") \
                    or ref.startswith("data:application/font") or ref.startswith("data:image/svg"):
                continue
            ext = os.path.splitext(ref.split("?")[0].split("#")[0])[1].lower()
            if ext in (".woff", ".woff2", ".ttf", ".otf", ".eot", ".svg", ".cur", ".ico"):
                continue
            img_entries.append(("<css>", ref, u))

    #cache time!11 ------------------------------------------------------------------
    unique = {}
    for tag, ref, ref_base in img_entries:
        key = ref if ref.startswith("data:") else urljoin(ref_base, ref)
        unique.setdefault(key, (ref, ref_base))
    if unique:
        with ThreadPoolExecutor(max_workers=16) as ex:
            futs = {ex.submit(page.image, ref, rb): key
                    for key, (ref, rb) in unique.items()}
            for f in as_completed(futs):
                f.result()
    mark("imgs", started)

    #rewrite HTML using cache ------------------------------------------------------
    def do_link(m):
        a = _attrs(m.group(0))
        if "stylesheet" not in a.get("rel", "").lower() or not a.get("href"):
            return m.group(0)
        media = a.get("media", "").lower()
        if media and "print" in media and "screen" not in media and "all" not in media:
            return ""
        href = htmllib.unescape(a["href"])
        if href.startswith("data:"):
            got = _decode_data_uri(href)
            css = page.process_css(got[0].decode("utf-8", "replace"), base) if got else ""
        else:
            u = urljoin(base, href)
            css = page.process_css(css_texts.get(u, ""), u)
        return "<style>%s</style>" % css if css else ""

    text = LINK_RE.sub(do_link, text)

    # inline <style> blocks: @import + url()
    text = STYLE_RE.sub(
        lambda m: m.group(1) + _rewrite_css_urls(page.process_css(m.group(2), base), page, base)
        + m.group(3), text)

    # style="...url(...)" attributes
    text = re.sub(r"""style\s*=\s*"([^"]*url\([^"]*)\"""",
                  lambda m: 'style="%s"' % _rewrite_css_urls(
                      page.process_css(m.group(1), base), page, base).replace('"', "'"),
                  text, flags=re.I)

    def do_img(m):
        tag = m.group(0)
        a = _attrs(tag)
        cands = []
        for k in ("src", "data-src", "data-original", "data-lazy-src"):
            if a.get(k):
                cands.append(a[k])
        if a.get("srcset") or a.get("data-srcset"):
            first = (a.get("srcset") or a.get("data-srcset")).split(",")[0].strip().split()
            if first:
                cands.append(first[0])
        for ref in cands:
            local = page.image(ref, base)
            if local:
                return _set_attr(tag, "src", local)
        return tag

    text = IMG_RE.sub(do_img, text)
    mark("html", started)
    return text.encode("utf-8", "replace")
