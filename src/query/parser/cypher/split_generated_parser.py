"""Keep ANTLR rule families reviewable without changing the generated translation unit."""
from pathlib import Path
import re


def split_parser(directory: Path) -> None:
    source = directory / 'CypherParser.cpp'
    text = source.read_text()
    # Idempotent for an existing split tree; normal generation supplies fresh text.
    text = re.sub(r'^#include "rules/([^"/]+\.inc)"\n',
                  lambda match: (directory / 'rules' / match[1]).read_text(), text, flags=re.M)
    groups = [('Statements', 'CypherContext'), ('Queries', 'QueryContext'),
              ('PatternsAndProjection', 'ProjectionBodyContext'), ('Expressions', 'ExpressionContext'),
              ('Names', 'VariableContext'), ('LiteralsAndComprehensions', 'LiteralContext')]
    offsets = [text.index('//----------------- '+rule+' ') for _, rule in groups]
    offsets.append(text.index('void CypherParser::initialize()'))
    assert offsets == sorted(offsets), 'ANTLR rule order changed; review grouping'
    folder = directory / 'rules'
    folder.mkdir(exist_ok=True)
    output = text[:offsets[0]]
    pieces = {}
    for index, (name, _) in enumerate(groups):
        body = text[offsets[index]:offsets[index+1]]
        if len(body.splitlines()) > 3000:
            raise ValueError(f'{name} exceeds 3000 lines; split its grammar responsibilities')
        pieces[name] = body
        output += f'#include "rules/{name}.inc"\n'
    output += text[offsets[-1]:]
    if len(output.splitlines()) > 3000:
        raise ValueError('ANTLR initialization exceeds 3000 lines; review static-data separation')
    expanded = re.sub(r'^#include "rules/([^"/]+)\.inc"\n', lambda m: pieces[m[1]], output, flags=re.M)
    assert expanded == text, 'generated token order changed'
    for name, body in pieces.items():
        (folder / (name+'.inc')).write_text(body)
    source.write_text(output)


if __name__ == '__main__':
    split_parser(Path(__file__).resolve().parent / 'generated')
