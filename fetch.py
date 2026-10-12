import pycurl

FIREFOX_UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:128.0) Gecko/20100101 Firefox/128.0"

def fetch_html(url):
    c = pycurl.Curl()
    c.setopt(pycurl.URL, url)
    c.setopt(pycurl.USERAGENT, FIREFOX_UA)
    c.setopt(pycurl.HTTPHEADER, ["Accept: text/html", "Accept-Language: en-US,en;q=0.9"])
    c.setopt(pycurl.FOLLOWLOCATION, 1)
    c.setopt(pycurl.TIMEOUT, 30)
    out = []
    c.setopt(pycurl.WRITEFUNCTION, out.append)
    c.perform()
    c.close()
    return b"".join(out)

def fetch_text(url):
    c = pycurl.Curl()
    c.setopt(pycurl.URL, url)
    c.setopt(pycurl.USERAGENT, FIREFOX_UA)
    c.setopt(pycurl.HTTPHEADER, ["Accept: text/plain", "Accept-Language: en-US,en;q=0.9"])
    c.setopt(pycurl.FOLLOWLOCATION, 1)
    c.setopt(pycurl.TIMEOUT, 30)
    out = []
    c.setopt(pycurl.WRITEFUNCTION, out.append)
    c.perform()
    c.close()
    return b"".join(out)

def fetch_image(url):
    c = pycurl.Curl()
    c.setopt(pycurl.URL, url)
    c.setopt(pycurl.USERAGENT, FIREFOX_UA)
    c.setopt(pycurl.HTTPHEADER, ["Accept: image/webp,image/apng,image/*,*/*;q=0.8"])
    c.setopt(pycurl.FOLLOWLOCATION, 1)
    c.setopt(pycurl.TIMEOUT, 30)
    out = []
    c.setopt(pycurl.WRITEFUNCTION, out.append)
    c.perform()
    c.close()
    return b"".join(out)
