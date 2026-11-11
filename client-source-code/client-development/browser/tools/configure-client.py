#!/usr/bin/env python3
# Writes page options (server address, keys, default themes, start page buttons) into a copy of a built client.html.
import argparse
import json
import os
import re
import sys
import uuid

DEFAULT_INPUT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "client.html"))
CIPHERS = ["aes256", "chacha20", "camellia256", "twofish256", "serpent256", "trivium80"]
CONFIG_SCRIPT = re.compile(rb"<script\b[^>]*\bid\s*=\s*[\"']lemon-cfg[\"'][^>]*>(.*?)</script\s*>", re.IGNORECASE | re.DOTALL)
THEME_BUTTON = re.compile(rb"<button\b[^>]*\bclass\s*=\s*[\"']choose-theme-item[\"'][^>]*>", re.IGNORECASE)
THEME_NAME = re.compile(rb"\bdata-name\s*=\s*[\"']([^\"']+)[\"']", re.IGNORECASE)
THEME_FOR_PHONE = re.compile(rb"\bdata-mobile\s*=\s*[\"']true[\"']", re.IGNORECASE)
SERVER_ADDRESS = re.compile(r"^[A-Za-z0-9.\-:\[\]]{1,255}$")


class ConfigureError(Exception):
    """A problem that is reported to the user in one line."""


def read_themes(page):
    """Reads the theme names the page contains. Each entry is (name, is_for_phone)."""
    themes = []
    for button in THEME_BUTTON.findall(page):
        name = THEME_NAME.search(button)
        if name:
            themes.append((name.group(1).decode("utf-8", "replace"), THEME_FOR_PHONE.search(button) is not None))
    return themes


def theme_label(theme):
    """The text printed for one theme: its name, marked when it is made for a phone."""
    return theme[0] + (" (phone)" if theme[1] else "")


def read_port(text):
    """Reads a whole number from 1 to 65535; used for --port, --wss-port and the typed answers."""
    try:
        port = int(text)
    except ValueError:
        port = 0
    if port < 1 or port > 65535:
        raise argparse.ArgumentTypeError("a port must be a whole number from 1 to 65535")
    return port


def read_key(text):
    """Turns "password" or "cipher:password" into the object the page expects."""
    cipher, separator, password = text.partition(":")
    if separator and cipher in CIPHERS:
        return {"type": cipher, "password": password}
    return {"type": "aes256", "password": text}


def read_options(arguments):
    """Reads the command line into a settings object."""
    parser = argparse.ArgumentParser(
        description="Writes page options into a copy of a built client.html. Without options the script asks for every value.",
        epilog="The connection passwords are readable by everybody who gets the file.")
    parser.add_argument("--input", default=DEFAULT_INPUT, help="a built client.html (default: client-source-code/client.html)")
    parser.add_argument("--output", default=None, help="the file to write (default: client-configured.html next to the input)")
    parser.add_argument("--host", default="", help="the server address written into the page; without it the page shows the connect window")
    parser.add_argument("--port", type=read_port, default=0, help="the server's WebSocket port")
    parser.add_argument("--wss-port", type=read_port, default=0, dest="wss_port", help="the port for secure connections, if it differs")
    parser.add_argument("--secure", action="store_true", dest="is_secure_websocket_used",
                        help="connect with wss to --wss-port (or --port) although the page is opened from a file")
    parser.add_argument("--key", action="append", default=[], type=read_key, dest="keys", metavar="[CIPHER:]PASSWORD",
                        help="a connection password, may be given several times; the cipher is one of " + ", ".join(CIPHERS) + " (default aes256)")
    parser.add_argument("--theme", default="", help="default theme on a desktop")
    parser.add_argument("--mobile-theme", default="", dest="mobile_theme", help="default theme on a phone")
    parser.add_argument("--connect-button", action="store_true", dest="is_connect_button_shown", help="show the start page with the connect button")
    parser.add_argument("--import-identity-button", action="store_true", dest="is_import_identity_button_shown",
                        help="show the import identity button under the connect button")
    parser.add_argument("--persist-identity", action="store_true", dest="is_identity_kept", help="keep each user's identity in the browser's storage")
    parser.add_argument("--list-themes", action="store_true", dest="is_theme_list_wanted", help="print the themes of the page and stop")
    return parser.parse_args(arguments)


def ask_yes(question):
    """Asks a yes or no question; an empty answer means no."""
    return input(question + " [y/N]: ").strip().lower() in ("y", "yes")


def read_typed_port(text):
    """Reads a port typed as an answer; a wrong one is reported in one line like every other problem."""
    try:
        return read_port(text)
    except argparse.ArgumentTypeError as error:
        raise ConfigureError(str(error))


def ask(settings):
    """Asks for every value on the terminal. An empty answer keeps the default shown in brackets."""
    answer = input("Built client.html to read [" + settings.input + "]: ").strip()
    settings.input = answer or settings.input
    answer = input("File to write [client-configured.html next to it]: ").strip()
    settings.output = answer or None
    if ask_yes("Write the server connection details into the page, so it connects without the connect window?"):
        settings.host = input("Server address (domain name or IP address): ").strip()
        settings.port = read_typed_port(input("WebSocket port: ").strip())
        settings.is_secure_websocket_used = ask_yes("Does the server accept only secure connections (wss)?")
        answer = input("Port for secure connections, if it differs (empty for none): ").strip()
        settings.wss_port = read_typed_port(answer) if answer else 0
        print("Connection passwords are readable by everybody who gets the file. Enter them one per line as password or cipher:password; an empty line ends the list.")
        answer = input("Connection password: ")
        while answer != "":
            settings.keys.append(read_key(answer))
            answer = input("Connection password: ")
    if os.path.isfile(settings.input):
        with open(settings.input, "rb") as file:
            print("Themes in this page: " + ", ".join(theme_label(theme) for theme in read_themes(file.read())))
    settings.theme = input("Default theme on a desktop (empty for the page's own): ").strip()
    settings.mobile_theme = input("Default theme on a phone (empty for the page's own): ").strip()
    settings.is_connect_button_shown = ask_yes("Show a connect button before the page connects (needed for sound without another click)?")
    settings.is_import_identity_button_shown = settings.is_connect_button_shown and ask_yes("Show an import identity button under it?")
    settings.is_identity_kept = ask_yes("Keep each user's identity in the browser, so the same identity is used on the next visit?")


def build_config(settings, themes):
    """Checks the settings against the page and builds the options object the page reads."""
    config = {}
    if settings.host:
        if not SERVER_ADDRESS.match(settings.host):
            raise ConfigureError("The server address may contain letters, digits, dots, hyphens, colons and square brackets only.")
        if not settings.port:
            raise ConfigureError("--host needs --port.")
        config["host"] = settings.host
        config["port"] = settings.port
        if settings.wss_port:
            config["wss_port"] = settings.wss_port
        elif settings.is_secure_websocket_used:
            # the page connects to wss_port when it uses a secure connection
            config["wss_port"] = settings.port
        if settings.is_secure_websocket_used:
            config["is_secure_websocket_used"] = True
        config["keys"] = settings.keys
    elif settings.port or settings.wss_port or settings.keys or settings.is_secure_websocket_used:
        raise ConfigureError("--port, --wss-port, --secure and --key need --host.")
    if settings.theme:
        if not any(name == settings.theme for name, is_for_phone in themes):
            raise ConfigureError("The page has no theme named " + settings.theme + ". Use --list-themes.")
        config["theme"] = settings.theme
    if settings.mobile_theme:
        if not any(name == settings.mobile_theme and is_for_phone for name, is_for_phone in themes):
            raise ConfigureError("The page has no theme for a phone named " + settings.mobile_theme + ". Use --list-themes.")
        config["mobile_theme"] = settings.mobile_theme
    if settings.is_import_identity_button_shown and not settings.is_connect_button_shown:
        raise ConfigureError("--import-identity-button needs --connect-button: the button is shown under the connect button.")
    config["is_connect_button_shown"] = settings.is_connect_button_shown
    config["is_import_identity_button_shown"] = settings.is_import_identity_button_shown
    config["persist_identity"] = settings.is_identity_kept
    return config


def configure(page, config):
    """Puts the options into the page and returns the new page."""
    script = CONFIG_SCRIPT.search(page)
    if not script:
        raise ConfigureError('This file has no script element "lemon-cfg". It is not a built client.html.')

    # the same text as JSON.stringify writes; "<" is escaped so the options cannot end the script element
    options = json.dumps(config, separators=(",", ":"), ensure_ascii=False).replace("<", "\\u003c")
    body = ("window.__SERVER_CONFIG__=" + options + ";").encode("utf-8")
    return page[:script.start(1)] + body + page[script.end(1):]


def write_atomic(path, content):
    """Writes the file through a temporary file, so a failed run leaves no half-written page."""
    folder = os.path.dirname(path)
    temporary = os.path.join(folder, ".lemon-client-" + uuid.uuid4().hex + ".tmp")
    os.makedirs(folder, exist_ok=True)
    try:
        with open(temporary, "xb") as file:
            file.write(content)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.remove(temporary)


def main(arguments):
    settings = read_options(arguments)
    if not arguments:
        if not sys.stdin.isatty():
            raise ConfigureError("No options given and no terminal to ask on. See --help.")
        ask(settings)
    if not os.path.isfile(settings.input):
        raise ConfigureError("Cannot find " + settings.input + ". Build the client first (build.bat or sh build.sh) or pass --input.")
    with open(settings.input, "rb") as file:
        page = file.read()
    themes = read_themes(page)
    if settings.is_theme_list_wanted:
        for theme in themes:
            print(theme_label(theme))
        return
    output = os.path.abspath(settings.output or os.path.join(os.path.dirname(settings.input), "client-configured.html"))
    if os.path.normcase(output) == os.path.normcase(os.path.abspath(settings.input)):
        raise ConfigureError("The output file must differ from the input file.")
    config = build_config(settings, themes)
    write_atomic(output, configure(page, config))
    print("Wrote " + output)
    if "host" in config:
        print("The page connects to " + config["host"] + ":" + str(config["port"]) + (" with wss" if config.get("is_secure_websocket_used") else "")
              + " and contains " + str(len(config["keys"])) + " connection password(s).")
    else:
        print("The page contains no server address: it shows the connect window.")
    print("Start page: connect button " + ("shown" if config["is_connect_button_shown"] else "not shown")
          + ", import identity button " + ("shown" if config["is_import_identity_button_shown"] else "not shown") + ".")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except ConfigureError as error:
        print("Could not configure the client: " + str(error), file=sys.stderr)
        sys.exit(1)
    except EOFError:
        # the input ended while a question waited for its answer, for example when the input is a file
        print("\nCould not configure the client: the input ended before every question was answered. See --help.", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nStopped.", file=sys.stderr)
        sys.exit(1)
