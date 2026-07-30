#!/usr/bin/env python3
"""Filter lcov coverage to only include project source files."""

from pathlib import Path
import re
import sys

def main():
    if len(sys.argv) != 3:
        print("Usage: filter_coverage.py <input.info> <output.info>")
        sys.exit(1)

    input_file = sys.argv[1]
    output_file = sys.argv[2]

    with open(input_file, 'r') as f:
        content = f.read()

    # Split into records (each starts with SF: and ends with end_of_record)
    records = re.split(r'(?=^SF:)', content, flags=re.MULTILINE)

    repo_root = Path(__file__).resolve().parents[1]

    def is_project_source(sf_line):
        path = Path(sf_line[3:]).resolve()
        try:
            rel = path.relative_to(repo_root)
        except ValueError:
            return False

        return (
            len(rel.parts) == 2
            and rel.parts[0] in {'src', 'include'}
            and rel.suffix in {'.cpp', '.h'}
        )

    filtered = []
    for record in records:
        lines = record.strip().split('\n')
        if lines and lines[0].startswith('SF:'):
            sf_line = lines[0]
            if is_project_source(sf_line):
                filtered.append(record)

    with open(output_file, 'w') as f:
        f.write(''.join(filtered))

    print(f"Filtered {len(records)} records to {len(filtered)} project files")

if __name__ == '__main__':
    main()
