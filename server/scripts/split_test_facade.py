"""SF-05: divide tests/test_server_facade.py em arquivos por dominio.

Por que um script em vez de edicao manual: o arquivo tem ~10.100 linhas e
~510 testes "flat" (sem classes/fixtures), entao a forma mais segura de
dividir sem alterar o corpo de nenhum teste e' fazer isso mecanicamente via
`ast` e deixar o pytest validar o resultado (contagem de testes preservada).

Uso (a partir de server/, com o ambiente de dev instalado):

    python scripts/split_test_facade.py            # dry-run, so mostra o plano
    python scripts/split_test_facade.py --apply     # escreve os novos arquivos
                                                      # e remove o original

Saida (--apply):
    tests/_facade_common.py     # helpers compartilhados (ex: _make_server_config)
    tests/test_facade_turns.py
    tests/test_facade_tools.py
    tests/test_facade_playback.py
    tests/test_facade_vision.py
    tests/test_facade_ops.py
    tests/test_server_facade.py  -> removido

Apos rodar com --apply, valide com:

    pytest -q
    pytest -q --collect-only | grep -c "::test_"   # deve bater com o total antigo

A categorizacao por palavra-chave e' uma heuristica (ordem de prioridade:
vision > playback > ops > tools > turns). Testes que cairem no arquivo
"errado" podem ser movidos manualmente depois -- o objetivo aqui e' apenas
quebrar o arquivo monolitico em pedacos manejaveis sem reescrever nenhum
corpo de teste.
"""

from __future__ import annotations

import argparse
import ast
import sys
from pathlib import Path

TESTS_DIR = Path(__file__).resolve().parent.parent / "tests"
SRC_PATH = TESTS_DIR / "test_server_facade.py"

# Nomes dos helpers de modulo (nao-teste) que ficam em _facade_common.py.
# Mantidos explicitos (em vez de descobertos automaticamente) para que o
# import explicito em cada arquivo de teste seja estavel e legivel.
COMMON_HELPER_NAMES = {
    "_make_server_config",
    "_server_loud_pcm",
    "_simulate_server_voice_session",
    "_wait_until",
    "_drain_queue",
}

# Heuristica de categorizacao por palavra-chave no nome do teste.
# Ordem importa: a primeira categoria cujo conjunto de keywords aparecer
# no nome (em qualquer lugar) vence.
CATEGORY_KEYWORDS: list[tuple[str, tuple[str, ...]]] = [
    (
        "vision",
        (
            "vision",
            "camera",
            "face_box",
            "presence",
            "ov2640",
        ),
    ),
    (
        "playback",
        (
            "audio",
            "playback",
            "opus",
            "codec",
            "tts",
            "piper",
            "stt",
            "say",
            "speech",
            "volume",
            "pcm",
            "afe",
            "barge",
            "vad",
            "mic",
            "i2s",
            "transcri",
            "whisper",
        ),
    ),
    (
        "ops",
        (
            "server_cli",
            "server_config",
            "server_entrypoint",
            "ops_",
            "diagnostics",
            "watchdog",
            "wifi",
            "ota",
            "web_service",
            "web_ota",
            "_http",
            "http_",
            "token",
            "bridge_",
            "agenda",
            "schedule",
            "nvs",
            "persistence",
            "boot",
            "service_manager",
            "circadian",
        ),
    ),
    (
        "tools",
        (
            "intent",
            "tool",
            "weather",
            "llm_",
            "_llm",
            "persona",
            "memory",
            "device_command",
            "personality",
        ),
    ),
]
DEFAULT_CATEGORY = "turns"

CATEGORY_FILES = {
    "turns": "test_facade_turns.py",
    "tools": "test_facade_tools.py",
    "playback": "test_facade_playback.py",
    "vision": "test_facade_vision.py",
    "ops": "test_facade_ops.py",
}

COMMON_MODULE = "_facade_common.py"


def categorize(name: str) -> str:
    lname = name.lower()
    for category, keywords in CATEGORY_KEYWORDS:
        if any(kw in lname for kw in keywords):
            return category
    return DEFAULT_CATEGORY


def node_span(node: ast.AST) -> tuple[int, int]:
    start = node.lineno
    decorators = getattr(node, "decorator_list", None)
    if decorators:
        start = min(start, decorators[0].lineno)
    end = node.end_lineno
    assert end is not None
    return start, end


def node_source(lines: list[str], node: ast.AST) -> str:
    start, end = node_span(node)
    return "".join(lines[start - 1 : end]).rstrip("\n") + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--apply", action="store_true", help="escreve os arquivos e remove o original")
    args = parser.parse_args()

    if not SRC_PATH.exists():
        print(f"nao encontrado: {SRC_PATH}", file=sys.stderr)
        return 1

    source = SRC_PATH.read_text(encoding="utf-8")
    lines = source.splitlines(keepends=True)
    tree = ast.parse(source, filename=str(SRC_PATH))

    import_lines: list[str] = []
    helper_nodes: list[ast.AST] = []
    test_nodes: list[ast.AST] = []
    other_nodes: list[ast.AST] = []

    for node in tree.body:
        if isinstance(node, (ast.Import, ast.ImportFrom)):
            start, end = node_span(node)
            import_lines.append("".join(lines[start - 1 : end]))
            continue
        if isinstance(node, ast.Expr) and isinstance(getattr(node, "value", None), ast.Constant):
            # docstring do modulo -- ignora
            continue
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            if node.name.startswith("test_"):
                test_nodes.append(node)
            elif node.name in COMMON_HELPER_NAMES:
                helper_nodes.append(node)
            else:
                print(f"AVISO: helper nao mapeado em COMMON_HELPER_NAMES: {node.name}", file=sys.stderr)
                helper_nodes.append(node)
            continue
        other_nodes.append(node)

    if other_nodes:
        kinds = ", ".join(type(n).__name__ for n in other_nodes)
        print(f"AVISO: statements de modulo inesperados ignorados: {kinds}", file=sys.stderr)

    header = "".join(import_lines).rstrip("\n") + "\n"

    buckets: dict[str, list[ast.AST]] = {cat: [] for cat in CATEGORY_FILES}
    for node in test_nodes:
        buckets[categorize(node.name)].append(node)

    print("Plano de divisao (contagem de testes por arquivo):")
    total = 0
    for cat, file_name in CATEGORY_FILES.items():
        count = len(buckets[cat])
        total += count
        print(f"  {file_name:28s} {count:4d} testes")
    print(f"  {'TOTAL':28s} {total:4d} testes (original: {len(test_nodes)})")
    assert total == len(test_nodes)

    if not args.apply:
        print("\n(dry-run -- use --apply para escrever os arquivos)")
        return 0

    # _facade_common.py: imports + helpers compartilhados
    common_body = "\n\n".join(node_source(lines, n).rstrip("\n") for n in helper_nodes)
    common_content = (
        '"""Helpers compartilhados extraidos de test_server_facade.py (SF-05)."""\n\n'
        f"{header}\n\n{common_body}\n"
    )
    (TESTS_DIR / COMMON_MODULE).write_text(common_content, encoding="utf-8")

    common_import_names = sorted(n.name for n in helper_nodes)
    common_import = f"from _facade_common import {', '.join(common_import_names)}\n"

    for cat, file_name in CATEGORY_FILES.items():
        nodes = buckets[cat]
        body = "\n\n".join(node_source(lines, n).rstrip("\n") for n in nodes)
        content = f"{header}\n{common_import}\n\n{body}\n"
        (TESTS_DIR / file_name).write_text(content, encoding="utf-8")

    SRC_PATH.unlink()

    print("\nArquivos escritos:")
    print(f"  {TESTS_DIR / COMMON_MODULE}")
    for file_name in CATEGORY_FILES.values():
        print(f"  {TESTS_DIR / file_name}")
    print(f"\nRemovido: {SRC_PATH}")
    print("\nValide com: pytest -q")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
