"""Compare the permission catalog with source and test immutable editing drafts."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


def array(text, marker):
    return json.JSONDecoder().raw_decode(text[text.index(marker) + len(marker):].lstrip())[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--jdk', type=Path, required=True)
    parser.add_argument('--json-jar', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    arguments = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    repo = project.parents[3]
    java = project / 'app/src/main/java/com/lemonchat'
    catalog = (java / 'ui/PermissionFields.java').read_text(encoding='utf-8')
    entries = {}
    for kind in ['SERVER', 'CHANNEL']:
        body = catalog.split('List<Field> ' + kind + ' =', 1)[1].split('    ));', 1)[0]
        entries[kind] = [json.loads('[' + line.strip()[10:-2] + ']') for line in body.splitlines() if line.strip().startswith('new Field(') and line.strip().endswith('),')]
        last = [line.strip() for line in body.splitlines() if line.strip().startswith('new Field(') and line.strip().endswith(')')]
        entries[kind].extend(json.loads('[' + line[10:-1] + ']') for line in last)
    server_source = (repo / 'server-source-code/main-server/main/core/permissions.c').read_text(encoding='utf-8')
    channel_source = (repo / 'server-source-code/main-server/main/core/channel_groups.c').read_text(encoding='utf-8')
    server_names = []
    for symbol in ['g_permission_names', 'g_permission_power_names']:
        body = re.search(re.escape(symbol) + r'\[[^]]+\]\s*=\s*\{([^}]+)\}', server_source).group(1)
        server_names.extend(re.findall(r'"([a-z_]+)"', body))
    channel_actions = re.search(r'boole channel_groups__supports\([^)]*\)\s*\{(.*?)\n\}', channel_source, re.S).group(1)
    channel_names = [name.lower() for name in re.findall(r'case PERMISSION_([A-Z_]+):', channel_actions)]
    channel_names.extend(re.findall(r'"([a-z_]+)"', re.search(r'g_channel_group_power_names\[[^]]+\]\s*=\s*\{([^}]+)\}', channel_source).group(1)))
    for kind, names in [('SERVER', server_names), ('CHANNEL', channel_names)]:
        actual = [entry[0] for entry in entries[kind]]
        assert len(actual) == len(set(actual)) and set(actual) == set(names), kind + ' permission catalog differs from the server'
    fallback = re.search(r'static uint64 _permissions_internal__action_value\([^)]*\)\s*\{(.*?)\n\}', server_source, re.S).group(1)
    allowed = {name.lower() for name in re.findall(r'permission == PERMISSION_([A-Z_]+)', fallback)}
    assert {entry[0] for entry in entries['SERVER'] if entry[6]} == allowed, 'Default action labels differ from the server'
    browser = repo / 'client-source-code/client-development/browser/main/scripts/app'
    for kind, source, variables in [('SERVER', browser / 'server/groups.js', ['groups_permission_fields', 'groups_power_fields']), ('CHANNEL', browser / 'channel/channel-groups.js', ['channel_groups_action_fields', 'channel_groups_power_fields'])]:
        text = source.read_text(encoding='utf-8')
        expected = {row[0]: row[1:] for variable in variables for row in array(text, 'var ' + variable + ' =')}
        for entry in entries[kind]:
            assert entry[1] == expected[entry[0]][0], 'Label differs from the browser: ' + entry[0]
            if len(expected[entry[0]]) > 1:
                assert entry[2] == expected[entry[0]][1], 'Explanation differs from the browser: ' + entry[0]
    print('PASS exact server, channel and browser permission catalog parity', flush=True)
    arguments.output.mkdir(parents=True, exist_ok=True)
    suffix = '.exe' if os.name == 'nt' else ''
    sources = [java / 'model/PermissionDefinition.java', java / 'ui/PermissionFields.java', java / 'ui/PermissionDraft.java', project / 'tests/permissions/PermissionUiHostProbe.java']
    subprocess.run([str(arguments.jdk / 'bin' / ('javac' + suffix)), '-encoding', 'UTF-8', '-cp', str(arguments.json_jar), '-d', str(arguments.output)] + list(map(str, sources)), check=True)
    subprocess.run([str(arguments.jdk / 'bin' / ('java' + suffix)), '-cp', os.pathsep.join(map(str, [arguments.output, arguments.json_jar])), 'com.lemonchat.ui.PermissionUiHostProbe'], check=True)


if __name__ == '__main__':
    main()
