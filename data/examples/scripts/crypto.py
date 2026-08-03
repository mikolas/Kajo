#!/usr/bin/env python3
# Bitcoin & Crypto Price Ticker for kajo
import urllib.request
import json

try:
    url = "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum&vs_currencies=usd"
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    with urllib.request.urlopen(req, timeout=3) as resp:
        data = json.loads(resp.read().decode())
        btc = data.get('bitcoin', {}).get('usd', 0)
        eth = data.get('ethereum', {}).get('usd', 0)
        print(f"BTC ${btc:,.0f} | ETH ${eth:,.0f}")
except Exception:
    print("Crypto Offline")
