import json, sys
src, dst = sys.argv[1], sys.argv[2]
j = json.load(open(src, encoding='utf-8'))
items = j['strings'] if isinstance(j, dict) and 'strings' in j else j


def esc(x):
    return x.replace('\\', '\\\\').replace('\n', '\\n')


with open(dst, 'w', encoding='utf-8', newline='\n') as f:
    f.write('# English\tRussian\n')
    for k, v in items.items():
        f.write(esc(k) + '\t' + esc(v) + '\n')
print('wrote', dst, len(items), 'entries')
