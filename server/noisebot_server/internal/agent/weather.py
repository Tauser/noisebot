from __future__ import annotations

from dataclasses import dataclass
import os
from urllib.parse import urlencode

from ..http_fetch import HttpFetchError, fetch_json


@dataclass(frozen=True)
class WeatherNow:
    temperature_c: float | None = None
    weather_code: int | None = None
    wind_kmh: float | None = None
    location: str = ""
    source: str = "Open-Meteo"


_WEATHER_LABELS = {
    0: "ceu limpo",
    1: "principalmente limpo",
    2: "parcialmente nublado",
    3: "nublado",
    45: "neblina",
    48: "neblina com geada",
    51: "garoa leve",
    53: "garoa moderada",
    55: "garoa forte",
    61: "chuva leve",
    63: "chuva moderada",
    65: "chuva forte",
    71: "neve leve",
    73: "neve moderada",
    75: "neve forte",
    80: "pancadas de chuva leves",
    81: "pancadas de chuva",
    82: "pancadas de chuva fortes",
    95: "trovoadas",
}


def _env_float(key: str, default: float) -> float:
    try:
        return float(os.environ.get(key, default))
    except (TypeError, ValueError):
        return default


def format_weather_reply(weather: WeatherNow) -> str:
    if weather.temperature_c is None:
        return "Nao consegui consultar o clima agora."
    location = weather.location or "sua localizacao configurada"
    label = _WEATHER_LABELS.get(weather.weather_code or -1)
    temp = int(round(weather.temperature_c))
    if label:
        return f"Agora em {location} esta {temp} graus, com {label}."
    return f"Agora em {location} esta {temp} graus."


def fetch_weather_now(timeout_s: float = 3.0) -> WeatherNow:
    lat = _env_float("NOISEBOT_WEATHER_LAT", -15.7939)
    lon = _env_float("NOISEBOT_WEATHER_LON", -47.8828)
    location = os.environ.get("NOISEBOT_WEATHER_LOCATION", "Brasilia")
    params = urlencode(
        {
            "latitude": lat,
            "longitude": lon,
            "current": "temperature_2m,weather_code,wind_speed_10m",
            "timezone": "auto",
        }
    )
    try:
        payload = fetch_json(
            f"https://api.open-meteo.com/v1/forecast?{params}",
            timeout_s=timeout_s,
        )
    except HttpFetchError:
        return WeatherNow(location=location)

    current = payload.get("current", {})
    temp = current.get("temperature_2m")
    code = current.get("weather_code")
    wind = current.get("wind_speed_10m")
    return WeatherNow(
        temperature_c=float(temp) if temp is not None else None,
        weather_code=int(code) if code is not None else None,
        wind_kmh=float(wind) if wind is not None else None,
        location=location,
    )
