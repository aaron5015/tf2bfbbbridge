#!/usr/bin/env python3
"""Register the hidden per-item BFBB bridge knockback attribute in TF2 items_game files.

Run from the TF2 bridge repository root:
    python tools/register_bridge_knockback_attribute.py

The schema files are several megabytes each, so this helper makes the small,
idempotent schema edit locally without replacing those large files through the
GitHub API. It does not assign tuning values to individual weapons.
"""
from pathlib import Path
import re
import sys

SCHEMA_PATHS = (
    Path("game/mod_tf/scripts/items/items_game.txt"),
    Path("game/items_game.txt"),
)
ATTRIBUTE_CLASS = "bridge_knockback_multiplier"

ENTRY_TEMPLATE = '''
\t\t"{id}"
\t\t{{
\t\t\t"name"\t"bridge knockback multiplier"
\t\t\t"attribute_class"\t"bridge_knockback_multiplier"
\t\t\t"description_string"\t""
\t\t\t"description_format"\t"value_is_percentage"
\t\t\t"hidden"\t"1"
\t\t\t"effect_type"\t"positive"
\t\t\t"stored_as_integer"\t"0"
\t\t}}
'''


def register(path: Path) -> bool:
    if not path.is_file():
        print(f"SKIP: {path} not found")
        return False

    text = path.read_text(encoding="utf-8")
    if f'"attribute_class"\t"{ATTRIBUTE_CLASS}"' in text or (
        f'"attribute_class" "{ATTRIBUTE_CLASS}' in text
    ):
        print(f"OK: {path} already defines {ATTRIBUTE_CLASS}")
        return False

    start = text.find('\n\t"attributes"\n\t{')
    if start < 0:
        raise RuntimeError(f"Could not locate top-level attributes block in {path}")

    section = text[start:]
    ids = [int(match.group(1)) for match in re.finditer(
        r'^\s*"(\d+)"\s*\n\s*\{', section, re.MULTILINE
    )]
    if not ids:
        raise RuntimeError(f"Could not find numeric attribute IDs in {path}")

    closing = "\n\t}\n}"
    if not text.endswith(closing):
        raise RuntimeError(f"Unexpected closing structure in {path}; left unchanged")

    attribute_id = max(ids) + 1
    entry = ENTRY_TEMPLATE.format(id=attribute_id)
    updated = text[:-len(closing)] + entry + closing
    path.write_text(updated, encoding="utf-8", newline="")
    print(f"UPDATED: {path} (attribute ID {attribute_id})")
    return True


def main() -> int:
    changed = 0
    for schema in SCHEMA_PATHS:
        try:
            changed += int(register(schema))
        except Exception as exc:
            print(f"ERROR: {exc}", file=sys.stderr)
            return 1
    if changed:
        print("Attribute registered. Add per-weapon overrides under each item's attributes block as desired.")
    else:
        print("No schema changes needed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
