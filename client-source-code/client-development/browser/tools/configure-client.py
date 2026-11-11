#!/usr/bin/env python3
# Writes page options (server address, keys, default themes, start page buttons) into a copy of a built client.html.
import argparse
import ctypes
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

# the marks in front of every printed line, the same as the server's setup script uses (main.c)
MARKS = {"info": "[*]", "ok": "[+]", "off": "[-]", "warn": "[!]", "ask": "[?]"}
MARK_COLORS = {"info": "1;36", "ok": "1;32", "off": "1;31", "warn": "1;33", "ask": "1;36"}


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


def enable_mark_colors():
    """
    @brief colours the marks when the output is a console that understands colour codes

    @return None
    """
    # only on a console
    if not sys.stdout.isatty():
        return

    # switch colour codes on
    if os.name == "nt":
        handle = ctypes.windll.kernel32.GetStdHandle(-11)
        mode = ctypes.c_uint32()
        if not ctypes.windll.kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
            return
        if not ctypes.windll.kernel32.SetConsoleMode(handle, mode.value | 0x0004):
            return

    # colour each mark
    for name in MARKS:
        MARKS[name] = "\033[" + MARK_COLORS[name] + "m" + MARKS[name] + "\033[0m"


def say(mark, text):
    """
    @brief prints one line with a mark in front of it

    @param str mark -> "info", "ok", "off", "warn" or "ask"
    @param str text -> the text of the line

    @return None
    """
    print(MARKS[mark] + " " + text)


def ask_text(question, default_text="", is_trimmed=True):
    """
    @brief asks for one line of text

    @param str question -> the question, without the colon
    @param str default_text -> shown in brackets and used for an empty answer
    @param bool is_trimmed -> False keeps spaces at both ends, for passwords

    @return str -> the answer, or the default
    """
    # show the default
    hint = " [" + default_text + "]" if default_text else ""

    # read the answer
    answer = input(MARKS["ask"] + " " + question + hint + ": ")
    if is_trimmed:
        answer = answer.strip()
    return answer or default_text


def ask_yes_no(question, note, default_answer):
    """
    @brief asks one yes or no question until the answer is y, n or empty

    @param str question -> the question, without the (y/n) hint
    @param list note -> lines printed under the question the first time, or None
    @param bool default_answer -> what an empty answer means

    @return bool -> True for yes
    """
    hint = "(Y/n): " if default_answer else "(y/N): "
    is_note_printed = False
    while True:
        # question with note
        if note and not is_note_printed:
            answer = input(MARKS["ask"] + " " + question + "\n" + "".join("    " + line + "\n" for line in note) + "    " + hint)
            is_note_printed = True
        else:
            answer = input(MARKS["ask"] + " " + question + " " + hint)
        answer = answer.strip().lower()

        # empty means default
        if answer == "":
            return default_answer

        # yes or no
        if answer in ("y", "yes"):
            return True
        if answer in ("n", "no"):
            return False

        # ask again
        say("warn", "Enter y or n, or press Enter to use the default.")


def ask_port(question):
    """
    @brief asks for a port until the answer is a whole number from 1 to 65535

    @param str question -> the question, without the colon

    @return int -> the port
    """
    while True:
        # read the answer
        answer = ask_text(question)

        # check the number
        try:
            return read_port(answer)
        except argparse.ArgumentTypeError:
            say("warn", "Enter a whole number from 1 to 65535.")


def ask_files(settings):
    """
    @brief asks which page to configure and which file to write

    @param object settings -> receives input and output

    @return bytes -> the content of the page to configure
    """
    page = b""
    default_input = settings.input

    # page to configure
    while True:
        settings.input = ask_text("Page to configure", default_input)
        if not os.path.isfile(settings.input):
            say("warn", "That file does not exist. Build the client first (build.bat or sh build.sh) or enter another path.")
            continue
        with open(settings.input, "rb") as file:
            page = file.read()
        if CONFIG_SCRIPT.search(page):
            break
        say("warn", "That file is not a built client.html.")

    # file to write
    default_output = os.path.join(os.path.dirname(os.path.abspath(settings.input)), "client-configured.html")
    while True:
        settings.output = ask_text("File to write", default_output)
        if os.path.normcase(os.path.abspath(settings.output)) != os.path.normcase(os.path.abspath(settings.input)):
            break
        say("warn", "The file to write must differ from the page to configure.")
    return page


def ask_passwords(settings):
    """
    @brief asks for the server's connection passwords, one at a time, each with its cipher

    @param object settings -> receives keys

    @return None
    """
    print()
    say("info", "Connection passwords")
    say("info", "Enter the server's connection passwords. Everybody who gets the file can read them.")
    while True:
        # next password
        number = len(settings.keys) + 1
        password = ask_text("Password " + str(number) + " (Enter for no more passwords)", "", False)
        if password == "":
            break

        # repeated password
        if any(key["password"] == password for key in settings.keys):
            say("warn", "This password is already in the list.")
            if not ask_yes_no("Add it again?", None, False):
                continue

        # cipher of the password
        while True:
            cipher = ask_text("Cipher: " + ", ".join(CIPHERS), CIPHERS[0])
            if cipher in CIPHERS:
                break
            say("warn", "Choose one of the listed cipher names.")

        # add to list
        settings.keys.append({"type": cipher, "password": password})
        say("ok", "Password " + str(number) + " added (" + cipher + ").")

    # no password given
    if not settings.keys:
        say("info", "No connection password is written into the page.")


def ask_server(settings):
    """
    @brief asks whether a server address is written into the page, and for its details

    @param object settings -> receives host, port, is_secure_websocket_used and keys

    @return None
    """
    print()
    say("info", "Server")

    # address wanted or not
    if not ask_yes_no("Write a server address into the page?", ["The page then connects without showing the connect window."], False):
        say("info", "No server address is written. The page shows the connect window.")
        return

    # server address
    while True:
        settings.host = ask_text("Server address (domain name or IP address)")
        if SERVER_ADDRESS.match(settings.host):
            break
        say("warn", "Use letters, digits, dots, hyphens, colons and square brackets only.")

    # kind of connection
    settings.is_secure_websocket_used = ask_yes_no("Use a secure connection (wss)?", ["Needed when the server accepts only secure connections."], False)

    # port
    settings.port = ask_port("Port of the secure connection" if settings.is_secure_websocket_used else "Port")
    say("ok", "Server: " + settings.host + ", port " + str(settings.port) + (", secure connection (wss)" if settings.is_secure_websocket_used else ""))

    # passwords
    ask_passwords(settings)


def ask_theme(themes, device, is_for_phone_wanted):
    """
    @brief shows the themes of the page with numbers and asks for the default theme of one kind of device

    @param list themes -> (name, is_for_phone) for every theme of the page
    @param str device -> "a computer" or "a phone", used in the texts
    @param bool is_for_phone_wanted -> True lists only the themes made for a phone

    @return str -> the chosen theme name, or "" to keep the page's default
    """
    names = [name for name, is_for_phone in themes if is_for_phone or not is_for_phone_wanted]

    # no theme to choose
    if not names:
        say("info", "No themes for " + device + " found in this page. Keeping the page's default.")
        return ""

    # numbered list
    say("info", "Themes available on " + device + ":")
    for index, name in enumerate(names):
        print("      " + str(index + 1) + ") " + name)
    while True:
        # read the answer
        answer = ask_text("Default theme on " + device + ", number or name (Enter to keep the page's default)")
        if answer == "":
            return ""

        # number to name
        if answer.isdigit() and 1 <= int(answer) <= len(names):
            answer = names[int(answer) - 1]

        # accept listed name
        if answer in names:
            say("ok", "Default theme on " + device + ": " + answer)
            return answer

        # ask again
        say("warn", "Enter a number from 1 to " + str(len(names)) + ", a listed name, or press Enter.")


def ask_start_page(settings):
    """
    @brief asks for the buttons of the start page and whether the identity is kept in the browser

    @param object settings -> receives is_connect_button_shown, is_import_identity_button_shown and is_identity_kept

    @return None
    """
    print()
    say("info", "Start page")

    # connect button
    settings.is_connect_button_shown = ask_yes_no("Show a Connect button before the page connects?",
        ["A browser plays no sound on a page until the user has clicked on it.", "With the button every user clicks once, so voices and sound effects are heard."], False)

    # import identity button
    if settings.is_connect_button_shown:
        settings.is_import_identity_button_shown = ask_yes_no("Show an Import identity button under it?", None, False)

    # identity in browser
    settings.is_identity_kept = ask_yes_no("Keep each user's identity in the browser?", ["The same identity is then used on the next visit."], False)


def print_summary(settings):
    """
    @brief prints every chosen value before the file is written

    @param object settings -> the answers

    @return None
    """
    rows = []

    # files
    rows.append(("page", settings.input))
    rows.append(("file to write", settings.output))

    # server and passwords
    if settings.host:
        rows.append(("server", settings.host + ", port " + str(settings.port) + (", secure connection (wss)" if settings.is_secure_websocket_used else "")))
        rows.append(("passwords", str(len(settings.keys)) + (" (" + ", ".join(key["type"] for key in settings.keys) + ")" if settings.keys else "")))
    else:
        rows.append(("server", "none, the page shows the connect window"))

    # themes
    rows.append(("theme, computer", settings.theme or "the page's default"))
    rows.append(("theme, phone", settings.mobile_theme or "the page's default"))

    # start page and identity
    rows.append(("start page", "Connect button: " + ("yes" if settings.is_connect_button_shown else "no")
        + ", Import identity button: " + ("yes" if settings.is_import_identity_button_shown else "no")))
    rows.append(("identity", "kept in the browser" if settings.is_identity_kept else "not kept in the browser"))

    # print the rows
    print()
    say("info", "Summary")
    for label, value in rows:
        print("      " + label.ljust(17) + value)


def ask(settings):
    """
    @brief asks for every value on the terminal, in the style of the server's setup script

    @param object settings -> receives the answers

    @return bool -> True when the file is to be written
    """
    # what this is
    say("info", "Lemon Chat page builder")
    say("info", "It writes a server address, passwords, default themes and the start page buttons into a copy of client.html.")
    print()

    # files
    page = ask_files(settings)

    # server
    ask_server(settings)

    # themes
    print()
    themes = read_themes(page)
    settings.theme = ask_theme(themes, "a computer", False)
    print()
    settings.mobile_theme = ask_theme(themes, "a phone", True)

    # start page
    ask_start_page(settings)

    # confirm
    print_summary(settings)
    return ask_yes_no("Write the file?", None, True)


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
    enable_mark_colors()

    # no options: ask
    if not arguments:
        if not sys.stdin.isatty():
            raise ConfigureError("No options given and no terminal to ask on. See --help.")
        if not ask(settings):
            say("off", "Nothing was written.")
            return

    # read the page
    if not os.path.isfile(settings.input):
        raise ConfigureError("Cannot find " + settings.input + ". Build the client first (build.bat or sh build.sh) or pass --input.")
    with open(settings.input, "rb") as file:
        page = file.read()
    themes = read_themes(page)

    # only list themes
    if settings.is_theme_list_wanted:
        for theme in themes:
            print(theme_label(theme))
        return

    # write the file
    output = os.path.abspath(settings.output or os.path.join(os.path.dirname(settings.input), "client-configured.html"))
    if os.path.normcase(output) == os.path.normcase(os.path.abspath(settings.input)):
        raise ConfigureError("The output file must differ from the input file.")
    config = build_config(settings, themes)
    write_atomic(output, configure(page, config))

    # say what was written
    say("ok", "Wrote " + output)
    if "host" in config:
        say("info", "The page connects to " + config["host"] + ":" + str(config["port"]) + (" with wss" if config.get("is_secure_websocket_used") else "")
            + " and contains " + str(len(config["keys"])) + " connection password(s).")
    else:
        say("info", "The page contains no server address: it shows the connect window.")
    say("info", "Start page: connect button " + ("shown" if config["is_connect_button_shown"] else "not shown")
        + ", import identity button " + ("shown" if config["is_import_identity_button_shown"] else "not shown") + ".")


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except ConfigureError as error:
        print(MARKS["warn"] + " Could not configure the client: " + str(error), file=sys.stderr)
        sys.exit(1)
    except EOFError:
        # the input ended while a question waited for its answer, for example when the input is a file
        print("\n" + MARKS["warn"] + " Could not configure the client: the input ended before every question was answered. See --help.", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\n" + MARKS["off"] + " Stopped. Nothing was written.", file=sys.stderr)
        sys.exit(1)
