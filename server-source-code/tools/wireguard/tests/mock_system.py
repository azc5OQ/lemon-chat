"""Stateful fake Linux commands used only by test_scripts.py."""

import json
from pathlib import Path
import sys

path = Path(sys.argv[1])
command, *args = sys.argv[2:]
state = json.loads(path.read_text())
result = 0

# Record fake firewall rules and optionally fail one insertion to exercise rollback.
if command == "iptables":
    args = args[2:]  # -w 10
    table = "filter"
    if args[:1] == ["-t"]:
        table, args = args[1], args[2:]
    action, chain, *parts = args
    if action == "-I":
        parts = parts[1:]  # insertion index
    rule = [table, chain, *parts]
    if action == "-C":
        result = 0 if rule in state["rules"] else 1
    elif action == "-S":
        pass
    elif action == "-I":
        if state.get("fail_port") in parts:
            state.pop("fail_port")
            result = 1
        else:
            state["rules"].append(rule)
    elif action == "-D":
        state["rules"].remove(rule)
    else:
        raise AssertionError(args)
elif command == "ip":
    if args[:2] == ["route", "get"]:
        print("1.1.1.1 via 192.0.2.1 dev eth0 src 192.0.2.2")
    elif args[:2] == ["link", "show"]:
        result = 0 if state["up"] else 1
    else:
        raise AssertionError(args)
elif command == "wg":
    if args[0] == "show":
        result = 0 if state["up"] else 1
    elif args[0] == "genkey":
        print("A" * 43 + "=")
    elif args[0] == "pubkey":
        sys.stdin.read()
        print("B" * 43 + "=")
    else:
        raise AssertionError(args)
elif command == "sysctl":
    if args[0] == "-n":
        print(state["forward"])
    else:
        state["forward"] = args[-1].split("=")[1]
elif command == "up":
    state["up"] = True
elif command == "down":
    state["up"] = False
elif command == "active":
    result = 0 if state["up"] else 1
elif command == "enabled":
    result = 0 if state["enabled"] else 1
elif command == "enable":
    state["enabled"] = True
elif command == "disable":
    state["enabled"] = False
else:
    raise AssertionError(command)

path.write_text(json.dumps(state))
sys.exit(result)
