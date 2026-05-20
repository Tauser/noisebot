from __future__ import annotations

from dataclasses import dataclass
import json
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen


@dataclass(frozen=True)
class MarketPrice:
    asset: str
    usd: float | None = None
    brl: float | None = None
    source: str = ""


def _format_money(value: float, symbol: str, decimals: int = 2) -> str:
    formatted = f"{value:,.{decimals}f}"
    formatted = formatted.replace(",", "_").replace(".", ",").replace("_", ".")
    return f"{symbol}{formatted}"


def format_btc_reply(price: MarketPrice) -> str:
    values = []
    if price.usd is not None:
        values.append(_format_money(price.usd, "US$ "))
    if price.brl is not None:
        values.append(_format_money(price.brl, "R$ "))
    if not values:
        return "Não consegui consultar a cotação do Bitcoin agora."
    joined = " ou ".join(values)
    source = f" Fonte: {price.source}." if price.source else ""
    return f"Bitcoin agora está em aproximadamente {joined}.{source}"


def fetch_btc_price(timeout_s: float = 3.0) -> MarketPrice:
    request = Request(
        "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=usd,brl",
        headers={"User-Agent": "NoiseBot-Bridge/1.0"},
    )
    try:
        with urlopen(request, timeout=timeout_s) as response:
            payload = json.loads(response.read().decode("utf-8"))
    except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError):
        return MarketPrice(asset="BTC", source="CoinGecko")

    bitcoin = payload.get("bitcoin", {})
    usd = bitcoin.get("usd")
    brl = bitcoin.get("brl")
    return MarketPrice(
        asset="BTC",
        usd=float(usd) if usd is not None else None,
        brl=float(brl) if brl is not None else None,
        source="CoinGecko",
    )
