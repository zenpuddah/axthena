#!/usr/bin/env python3
"""Independent standard-library replay of retained public API text projections."""
import argparse
import hashlib
import html
from html.parser import HTMLParser
import json
from pathlib import Path

class Projection(HTMLParser):
    blocks = set('p div section article h1 h2 h3 h4 h5 h6 ul ol li blockquote figure figcaption table tr br hr'.split())
    omitted = set('script style nav aside form button noscript iframe svg video audio'.split())

    def __init__(self, fragment):
        super().__init__(convert_charrefs=False)
        self.parts = []
        self.skip = None
        self.feed(fragment)
        self.close()
        assert self.skip is None

    def handle_starttag(self, tag, attrs):
        if self.skip:
            return
        if tag in self.omitted:
            self.skip = tag
            self.parts.append('\n')
        elif tag in self.blocks:
            self.parts.append('\n')
        elif tag in ('td', 'th'):
            self.parts.append(' ')

    def handle_endtag(self, tag):
        if self.skip:
            if tag == self.skip:
                self.skip = None
                self.parts.append('\n')
        elif tag in self.blocks:
            self.parts.append('\n')
        elif tag in ('td', 'th'):
            self.parts.append(' ')

    def handle_data(self, data):
        if not self.skip:
            self.parts.append(''.join(' ' if c in '\t\n\r\f\v' else c for c in data))

    def handle_entityref(self, name):
        if not self.skip:
            self.parts.append(html.unescape('&' + name + ';').replace('\u00a0', ' '))

    def handle_charref(self, name):
        if not self.skip:
            self.parts.append(html.unescape('&#' + name + ';').replace('\u00a0', ' '))

    def text(self):
        lines = [' '.join(x.split(' ')).strip(' ') for x in ''.join(self.parts).split('\n')]
        return '\n'.join(' '.join(word for word in line.split(' ') if word) for line in lines if line)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--store', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    imported = json.loads(args.input.read_bytes())

    def evidence(ref):
        raw = (args.store/'evidence'/(ref['sha256']+'.bin')).read_bytes()
        assert len(raw) == ref['bytes'] and hashlib.sha256(raw).hexdigest() == ref['sha256']
        return raw

    rows = []
    for report in imported['reports']:
        item = json.loads(evidence(report['capture_metadata']))
        response = json.loads(evidence(report['response']))
        assert item in response
        projected = Projection(item['content']['rendered']).text()
        assert projected == report['body_text'], report['number']
        assert projected.encode() == evidence(report['body'])
        assert hashlib.sha256(projected.encode()).hexdigest() == report['body_sha256']
        assert Projection(item['title']['rendered']).text() == report['title']
        assert report['published_date'] == item['date'][:10]
        assert report['source_available_at'] is None
        rows.append({'number': report['number'], 'title': report['title'], 'date': report['published_date'],
                     'url': report['url'], 'utf8_bytes': len(projected.encode()), 'body_sha256': report['body_sha256'],
                     'first_200': projected[:200], 'last_200': projected[-200:]})
    result = {'input_sha256': hashlib.sha256(args.input.read_bytes()).hexdigest(),
              'verified_reports': len(rows), 'empty_bodies': [r['number'] for r in rows if not r['utf8_bytes']],
              'scope': 'Independent parser replay and byte lineage; not a claim of full publisher-page completeness', 'reports': rows}
    with args.out.open('x') as output:
        json.dump(result, output, indent=2)
        output.write('\n')
    print(f"Verified {len(rows)} public source projections; empty bodies: {result['empty_bodies']}")

if __name__ == '__main__':
    main()
