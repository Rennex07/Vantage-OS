"""Invalidate kernel objects when command-line build configuration changes."""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True)
    for name in ('cc', 'ld', 'objcopy', 'cflags', 'firmware'):
        parser.add_argument('--' + name, required=True)
    args = vars(parser.parse_args())
    output = args.pop('output')
    content = json.dumps(args, sort_keys=True, indent=2) + '\n'
    if not output.exists() or output.read_text(encoding='utf-8') != content:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(content, encoding='utf-8')


if __name__ == '__main__':
    main()
