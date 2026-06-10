"""Extract plain text from resume / job-description files (.txt .md .pdf .docx)."""
from __future__ import annotations

import os


def read_text(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    if ext == ".pdf":
        try:
            from pypdf import PdfReader
        except ImportError as e:
            raise SystemExit("PDF support needs pypdf: pip install pypdf") from e
        reader = PdfReader(path)
        text = "\n".join((page.extract_text() or "") for page in reader.pages)
        if not text.strip():
            raise SystemExit(
                f"{path}: no extractable text — the PDF looks image-based. "
                "ATS systems will choke on it too; export a text-based PDF.")
        return text
    if ext == ".docx":
        try:
            import docx  # python-docx
        except ImportError as e:
            raise SystemExit("DOCX support needs python-docx: pip install python-docx") from e
        d = docx.Document(path)
        return "\n".join(p.text for p in d.paragraphs)
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()
