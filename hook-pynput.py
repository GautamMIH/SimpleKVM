# hook-pynput.py
from PyInstaller.utils.hooks import collect_submodules
hiddenimports = collect_submodules('pynput.keyboard._win32')
hiddenimports += collect_submodules('pynput.mouse._win32')