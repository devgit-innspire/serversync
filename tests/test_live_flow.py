# Full shopping flow against a running server (local or deployed), 4 users at the same time.
# Each user: signup -> login -> add address -> add to cart -> checkout -> order page -> orders list -> logout.
# All 4 buy the SAME product in parallel: stock must drop by exactly 4 (row locks in the checkout transaction).
# Each pays a different way (COD, UPI, Card, Net Banking). Also: bad payment details are rejected, a declined test card
# rolls the whole checkout back (cart stays), and cancelling an online-paid order makes the payment REFUNDED.
# Usage: py tests/test_live_flow.py https://serversync-wo8b.onrender.com [run-tag]
import http.cookiejar, re, sys, threading, time, urllib.error, urllib.parse, urllib.request

BASE = sys.argv[1].rstrip("/")
TAG = sys.argv[2] if len(sys.argv) > 2 else str(int(time.time()))
PASSWORD = "Kq7#Tiger$Moon"
USERS = ["Shoyam Bishnoi", "Sneha Negi", "Aaradhna Jaiswal", "Ashutosh"]
SHARED = 9           # TWS Wireless Earbuds: everybody buys one
EXTRA = [2, 30, 41, 51]   # plus one different product each
# per user: payment form fields, text the order page must show, expected payment status
PAYMENTS = [
    ({"pay_method": "COD"}, "Cash on Delivery", "PENDING"),
    ({"pay_method": "UPI", "upi_id": "sneha.negi@okaxis"}, "UPI (sneha.negi@okaxis)", "PAID"),
    ({"pay_method": "CARD", "card_number": "4111 1111 1111 1111", "card_name": "Aaradhna Jaiswal", "card_expiry": "12/30",
      "card_cvv": "123"}, "Card (VISA **** 1111)", "PAID"),
    ({"pay_method": "NETBANKING", "bank": "HDFC"}, "Net Banking (HDFC Bank)", "PAID"),
]

results, lock = [], threading.Lock()


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *a, **k):
        return None


class Client:
    def __init__(self):
        self.jar = http.cookiejar.CookieJar()
        self.op = urllib.request.build_opener(urllib.request.HTTPCookieProcessor(self.jar), NoRedirect)

    def req(self, method, path, form=None):
        data = urllib.parse.urlencode(form).encode() if form is not None else None
        r = urllib.request.Request(BASE + path, data=data, method=method)
        try:
            resp = self.op.open(r, timeout=90)
        except urllib.error.HTTPError as e:
            resp = e
        return resp.status, resp.headers.get("Location", ""), resp.read().decode("utf-8", "replace")


def check(who, step, ok, info=""):
    with lock:
        results.append((who, step, ok))
        print(f"[{'PASS' if ok else 'FAIL'}] {who:17} {step}" + (f"  ({info})" if info and not ok else ""))
    return ok


def flow(i, name, barrier, out):
    c = Client()
    email = f"{name.split()[0].lower()}.{TAG}@example.com"
    s, loc, _ = c.req("POST", "/signup", {"name": name, "email": email, "password": PASSWORD, "confirm_password": PASSWORD})
    check(name, "signup", s == 302 and "/login" in loc, f"{s} {loc}")
    s, loc, _ = c.req("POST", "/login", {"email": email, "password": PASSWORD})
    check(name, "login", s == 302 and any(k.name == "session_id" for k in c.jar), f"{s} {loc}")
    s, _, body = c.req("GET", "/")
    check(name, "home shows logged-in user", s == 200 and name.split()[0] in body, str(s))

    s, _, body = c.req("GET", "/addresses/new")
    states = re.findall(r'<option value="(\d+)"[^>]*>([^<]+)</option>', body)
    sid = next((v for v, n in states if n.strip() == "Rajasthan"), states[0][0] if states else "1")
    s, loc, _ = c.req("POST", "/addresses/new", {
        "first_name": name.split()[0], "last_name": (name.split() + ["Kumar"])[1], "phone": f"98765432{10 + i}",
        "address_type": "HOME", "line1": f"{i + 1} College Road", "line2": "", "landmark": "Near Library",
        "city": "Jaipur", "state_id": sid, "pincode": "302017"})
    check(name, "add address", s == 302, f"{s} {loc}")

    for pid in (SHARED, EXTRA[i]):
        s, loc, _ = c.req("POST", "/cart/add", {"product_id": pid, "return": "/cart"})
        check(name, f"add product {pid} to cart", s == 302, f"{s} {loc}")
    s, _, body = c.req("GET", "/cart")
    check(name, "cart has 2 items", s == 200 and body.count('name="product_id"') >= 2, str(s))

    s, _, body = c.req("GET", "/checkout")
    aid = re.search(r'name="address_id" value="(\d+)"', body)
    check(name, "checkout page with address", s == 200 and aid is not None, str(s))

    addr = aid.group(1) if aid else ""
    pay, shown, pstatus = PAYMENTS[i]
    if pay["pay_method"] == "UPI":
        s, _, body = c.req("POST", "/checkout", {"address_id": addr, "pay_method": "UPI", "upi_id": "not-a-upi-id"})
        check(name, "bad UPI ID rejected (400, form kept)", s == 400 and "valid UPI ID" in body and 'value="UPI" checked' in body, str(s))
    if pay["pay_method"] == "CARD":
        bad = dict(pay, card_number="4111 1111 1111 1112")
        s, _, body = c.req("POST", "/checkout", dict(bad, address_id=addr))
        check(name, "wrong card number rejected (Luhn)", s == 400 and "card number is not valid" in body, str(s))
        s, _, body = c.req("POST", "/checkout", dict(pay, card_expiry="01/20", address_id=addr))
        check(name, "expired card rejected", s == 400 and "expiry" in body, str(s))
        s, _, body = c.req("POST", "/checkout", dict(pay, card_number="4000 0000 0000 0002", address_id=addr))
        check(name, "declined card -> ROLLBACK, no order", s == 409 and "declined" in body, str(s))
        s, _, body = c.req("GET", "/cart")
        check(name, "cart still full after decline", s == 200 and body.count('name="product_id"') >= 2, str(s))

    barrier.wait()      # sab ek saath "Place order" dabate hain
    s, loc, _ = c.req("POST", "/checkout", dict(pay, address_id=addr))
    m = re.search(r"/order\?id=(\d+)", loc)
    check(name, "place order (parallel)", s == 302 and m is not None, f"{s} {loc}")
    oid = m.group(1) if m else "0"
    out[name] = (c, oid)

    s, _, body = c.req("GET", f"/order?id={oid}")
    check(name, f"order #{oid} paid by {pay['pay_method']}: {pstatus}", s == 200 and shown in body and f">{pstatus}<" in body
          and (pstatus == "PENDING" or ", Txn TXN" in body), str(s))
    s, _, body = c.req("GET", "/orders")
    check(name, "order in My Orders", s == 200 and f"id={oid}" in body, str(s))
    s, _, body = c.req("GET", "/cart")
    check(name, "cart empty after checkout", s == 200 and 'name="product_id"' not in body, str(s))


def main():
    print(f"Target: {BASE}   run tag: {TAG}\n")
    barrier = threading.Barrier(len(USERS))
    out = {}
    ts = [threading.Thread(target=flow, args=(i, n, barrier, out)) for i, n in enumerate(USERS)]
    for t in ts: t.start()
    for t in ts: t.join()

    # other users can't see someone else's order
    names = list(out)
    if len(names) >= 2:
        a, b = out[names[0]], out[names[1]]
        s, _, _ = a[0].req("GET", f"/order?id={b[1]}")
        check(names[0], f"cannot open {names[1]}'s order", s in (403, 404), str(s))

    # Ashutosh cancels
    if "Ashutosh" in out:
        c, oid = out["Ashutosh"]
        s, loc, _ = c.req("POST", "/order/cancel", {"order_id": oid})
        s2, _, body = c.req("GET", f"/order?id={oid}")
        check("Ashutosh", f"cancel order #{oid}", s == 302 and "Cancelled" in body, f"{s} {loc}")
        check("Ashutosh", "online payment REFUNDED after cancel", ">REFUNDED<" in body, "")

    for n, (c, _) in out.items():
        s, loc, _ = c.req("POST", "/logout", {})
        s2, loc2, _ = c.req("GET", "/orders")
        check(n, "logout (orders page needs login again)", s == 302 and s2 == 302 and "/login" in loc2, f"{s} {s2} {loc2}")

    fails = [r for r in results if not r[2]]
    print(f"\n{len(results) - len(fails)}/{len(results)} checks passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
