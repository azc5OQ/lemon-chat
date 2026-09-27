"""Run with python test_scripts.py --shell /bin/dash (or BusyBox sh).

All privileged commands are mocked; /etc and /run are relocated to a temporary
directory. No real network, package, firewall or service operations are run.
"""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
SHELL = "sh"


def invoke(code, *args):
    return subprocess.run([SHELL, "-c", code, "test", *args], text=True,
                          capture_output=True, timeout=30)


class Scripts(unittest.TestCase):
    def test_syntax(self):
        for script in ROOT.rglob("*.sh"):
            result = subprocess.run([SHELL, "-n", str(script)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, (script, result.stderr))

    def test_validation(self):
        for function, good, bad in [
            ("valid_port", ["1", "65535"], ["0", "65536", "-1", "08", "22;id", "999999999999"]),
            ("valid_ipv4", ["1.2.3.4", "10.66.0.2"], ["999.1.1.1", "1.2.3", "01.2.3.4", "1.2.3.4\nx"]),
            ("valid_subnet", ["10.66.0.0/24", "192.168.4.0/30", "172.16.0.0/12"],
             ["10.66.0.128/25", "10.66.0.0/32", "10.66.0.0/0", "8.0.0.0/8", "192.168.1.0/16", "999.0.0.0/24"]),
            ("valid_ports", ["1111,443", "3478,20000:20099"], ["", "1,", "0", "22;id", "9:2", "1:2:3"]),
        ]:
            for value in good + bad:
                result = invoke('. "$1"; "$2" "$3"', str(ROOT / "router/validate.sh"), function, value)
                self.assertEqual(result.returncode == 0, value in good, (function, value, result.stderr))
        for tcp, udp, expected in [("22", "3478", False), ("1111", "50000:60000", False), ("1111", "20000:20099", True)]:
            result = invoke('. "$1"; MODE=ports; WG_PORT=51820; SSH_PORT=22; SUBNET=10.66.0.0/24; TCP_PORTS=$2; UDP_PORTS=$3; validate_tunnel', str(ROOT / "router/validate.sh"), tcp, udp)
            self.assertEqual(result.returncode == 0, expected)

    def test_shell_quote(self):
        source = ROOT / "common.sh"
        # common.sh resolves its location using the entry point's $0.
        for value in ["", "spaces ' quote; $(printf BAD)", 'x"y\\z', "line1\nline2\n"]:
            result = subprocess.run([SHELL, "-c", '. "$1"; shell_quote "$2"', str(source), str(source), value], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            decoded = invoke("printf '%s' " + result.stdout)
            self.assertEqual(decoded.stdout, value, decoded.stderr)

    def test_check_errors_and_silent_connection(self):
        source = (ROOT / "router/check.sh").read_text()
        source = source.replace('. "$(dirname "$0")/validate.sh"', '. ' + shlex.quote(str(ROOT / "router/validate.sh")))
        for error, connections, success in [(3, 0, False), (6, 0, False), (7, 0, False), (28, 0, False), (28, 1, True), (52, 1, True), (0, 1, True)]:
            stub = f'curl() {{ printf %s {connections}; return {error}; }}\n'
            result = invoke(stub + 'set -- 10.66.0.2 1111\n' + source)
            self.assertEqual(result.returncode == 0, success, result.stderr)
            self.assertEqual("TUNNEL_TCP_OK" in result.stdout, success)

    def test_atomic_settings_failure(self):
        spec = importlib.util.spec_from_file_location("json_file", ROOT / "json_file.py")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "settings.json"
            original = '{"keep":123}'
            path.write_text(original)
            with mock.patch.object(module.os, "replace", side_effect=OSError("disk error")):
                with self.assertRaises(OSError): module.write(str(path), ["new=true"], True)
            self.assertEqual(path.read_text(), original)
            self.assertEqual(len(list(Path(directory).iterdir())), 1)
            module.write(str(path), ["new=true"], True)
            self.assertEqual(json.loads(path.read_text()), {"keep": 123, "new": True})

    def test_firewalld_policy_scope_and_cleanup(self):
        with tempfile.TemporaryDirectory(prefix="wg-fw-test-") as directory:
            base = Path(directory).resolve()
            source = (ROOT / "router/firewalld.sh").read_text().replace("/etc/", base.as_posix() + "/etc/").replace("firewall-cmd", "mock_firewall_cmd")
            prefix = 'set -eu; mock_firewall_cmd() { return 0; }; ZONE=external; HOST_IP=10.66.0.2; TCP_PORTS="1111 443"; UDP_PORTS="20000:20099"; WG_PORT=51820\n'
            result = invoke(prefix + 'ACTION=up\n' + source)
            self.assertEqual(result.returncode, 0, result.stderr)
            folder = base / "etc/firewalld/policies"
            inbound = ET.parse(folder / "tunnel-wg0-in.xml").getroot()
            self.assertEqual(inbound.find("ingress-zone").get("name"), "external")
            self.assertEqual(inbound.find("egress-zone").get("name"), "tunnel-wg0")
            self.assertEqual([rule.find("port").get("port") for rule in inbound.findall("rule")], ["1111", "443", "20000-20099"])
            self.assertTrue(all(rule.find("destination").get("address") == "10.66.0.2" for rule in inbound.findall("rule")))
            existing = folder / "administrator.xml"
            existing.write_text("unrelated")
            result = invoke(prefix + 'ACTION=down\n' + source)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(list(folder.iterdir()), [existing])
            # Never overwrite a colliding, unowned policy.
            (folder / "tunnel-wg0-in.xml").write_text("unrelated")
            result = invoke(prefix + 'ACTION=up\n' + source)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual((folder / "tunnel-wg0-in.xml").read_text(), "unrelated")

    def test_router_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="wg-test-") as directory:
            base = Path(directory).resolve()
            env = base / "env.sh"
            state = base / "state.json"
            mocker = Path(__file__).with_name("mock_system.py")
            state.write_text(json.dumps({"rules": [], "up": False, "enabled": False, "forward": "0"}))
            functions = '''
mock_cmd() { PYTHON MOCK STATE "$@"; }
id() { printf '0\\n'; }
ip() { mock_cmd ip "$@"; }
wg() { mock_cmd wg "$@"; }
iptables() { mock_cmd iptables "$@"; }
sysctl() { mock_cmd sysctl "$@"; }
curl() { return 0; }
mock_firewall_cmd() { return 1; }
mock_wg_quick() {
    if [ "$1" = up ]; then
        mock_cmd up
        if sh FORWARD up; then return 0; else mock_cmd down; return 1; fi
    else
        sh FORWARD down || return 1
        mock_cmd down
    fi
}
systemctl() {
    case "$1" in
        is-active) mock_cmd active ;;
        is-enabled) mock_cmd enabled ;;
        enable) mock_cmd enable ;;
        disable) mock_cmd disable ;;
        start) mock_wg_quick up ;;
        stop) mock_wg_quick down ;;
        restart) mock_wg_quick down && mock_wg_quick up ;;
        *) return 1 ;;
    esac
}
sh() { REALSH -c '. "$1"; shift; f=$1; shift; . "$f"' test ENV "$@"; }
'''
            for token, value in [("PYTHON", sys.executable), ("MOCK", str(mocker)), ("STATE", str(state)), ("REALSH", SHELL), ("ENV", str(env)), ("FORWARD", str(base / "etc/wireguard/wg0-forward.sh"))]:
                functions = functions.replace(token, shlex.quote(value.replace("\\", "/")))
            env.write_text(functions, newline="\n")
            scripts = base / "scripts"
            scripts.mkdir()
            (base / "run/systemd/system").mkdir(parents=True)
            for script in (ROOT / "router").glob("*.sh"):
                text = script.read_text().replace("/etc/", base.as_posix() + "/etc/").replace("/run/", base.as_posix() + "/run/")
                # POSIX functions cannot contain hyphens; rename mocked commands in test copies.
                text = text.replace("firewall-cmd", "mock_firewall_cmd").replace("wg-quick", "mock_wg_quick")
                if script.name in ("setup.sh", "teardown.sh"):
                    text = text.replace("set -eu", '. ' + shlex.quote(env.as_posix()) + '\nset -eu', 1)
                (scripts / script.name).write_text(text, newline="\n")
            def setup(port):
                return subprocess.run([SHELL, str(scripts / "setup.sh"), "--host-pubkey", "A" * 43 + "=", "--tcp-ports", str(port), "--udp-ports", "3478,20000:20099"], capture_output=True, text=True, timeout=60)
            def read(): return json.loads(state.read_text())
            # First install and rerun must not grow the rule set.
            result = setup(1111)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            count = len(read()["rules"])
            result = setup(1111)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(len(read()["rules"]), count)
            # Existing administrator rules survive setup and teardown.
            data = read(); data["rules"].append(["filter", "INPUT", "-p", "tcp", "--dport", "9999", "-j", "ACCEPT"]); state.write_text(json.dumps(data))
            result = setup(2222)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('"1111"', json.dumps(read()["rules"]))
            self.assertIn('"2222"', json.dumps(read()["rules"]))
            self.assertTrue(all(rule[0] == "mangle" for rule in read()["rules"] if "TCPMSS" in rule))
            # Inject one failed insertion; success marker is forbidden and old config must return.
            data = read(); data["fail_port"] = "3333"; state.write_text(json.dumps(data))
            result = setup(3333)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("SETUP_OK=1", result.stdout)
            self.assertTrue(read()["up"], result.stdout + result.stderr)
            self.assertIn('"2222"', json.dumps(read()["rules"]))
            self.assertNotIn('"3333"', json.dumps(read()["rules"]))
            result = subprocess.run([SHELL, str(scripts / "teardown.sh")], capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(len(read()["rules"]), 1)
            self.assertFalse(read()["up"])
            self.assertFalse(read()["enabled"])
            self.assertEqual(read()["forward"], "0")
            # Refuse to overwrite or stop somebody else's tunnel.
            config = base / "etc/wireguard/wg0.conf"
            config.write_text("# somebody else's tunnel\n")
            result = setup(1111)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(config.read_text(), "# somebody else's tunnel\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--shell", default="sh")
    args, remaining = parser.parse_known_args()
    SHELL = args.shell
    unittest.main(argv=[sys.argv[0], *remaining])
