"""Run the independent host-side native regression checks."""
from pathlib import Path
import subprocess
import sys

directory = Path(__file__).resolve().parent
for name in ['state_transport_test.py', 'callback_test.py', 'call_test.py', 'publication_test.py', 'settings_action_test.py', 'connection_wizard_test.py', 'runtime_assets_test.py', 'file_save_test.py', 'permission_chain_test.py', 'foreground_service_test.py']:
    subprocess.run([sys.executable, str(directory / name)], check=True)

