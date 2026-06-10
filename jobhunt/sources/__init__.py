"""Adapter registry: maps config `type` strings to Source classes."""
from __future__ import annotations

from .base import Source
from .workday import WorkdaySource
from .greenhouse import GreenhouseSource
from .lever import LeverSource
from .eightfold import EightfoldSource
from .custom import (
    AmazonSource, GoogleSource, MicrosoftSource, TeslaSource, AppleSource,
)

REGISTRY: dict[str, type[Source]] = {
    "workday": WorkdaySource,
    "greenhouse": GreenhouseSource,
    "lever": LeverSource,
    "eightfold": EightfoldSource,
    "amazon": AmazonSource,
    "google": GoogleSource,
    "microsoft": MicrosoftSource,
    "tesla": TeslaSource,
    "apple": AppleSource,
}


def build_sources(companies: list[dict]) -> list[Source]:
    sources = []
    for entry in companies:
        entry = dict(entry)
        if entry.pop("enabled", True) is False:
            continue
        kind = entry.pop("type")
        name = entry.pop("name")
        cls = REGISTRY.get(kind)
        if cls is None:
            raise ValueError(f"unknown source type {kind!r} for {name}")
        sources.append(cls(name, **entry))
    return sources
