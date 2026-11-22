# -*- mode: python ; coding: utf-8 -*-
import os


a = Analysis(
    ['c4u_launcher_qt.py'],
    pathex=[],
    binaries=[],
    datas=[('assets/caravan4u.ico', 'assets'), ('assets/header.jpg', 'assets')],
    hiddenimports=['c4u_options', 'c4u_ini', 'c4u_updater', 'c4u_form', 'c4u_logic', 'c4u_mods'],
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[
        'tkinter', 'customtkinter', 'darkdetect', 'glass', 'mh3u_launcher', 'mh3u_launcher_qt', 'core', 'numpy',
        'PySide6.QtNetwork', 'PySide6.QtQml', 'PySide6.QtQuick', 'PySide6.QtPdf', 'PySide6.QtSvg', 'PySide6.QtOpenGL',
        'PySide6.QtDBus', 'PySide6.QtWebEngineCore', 'PySide6.QtMultimedia', 'PySide6.QtSql', 'PySide6.QtTest',
    ],
    noarchive=False,
    optimize=0,
)
# Qt libraries the launcher never loads (software OpenGL alone is ~20 MB)
_UNUSED = ('opengl32sw', 'Qt6Network', 'Qt6Qml', 'Qt6Quick', 'Qt6Pdf', 'Qt6Svg', 'Qt6OpenGL', 'Qt6VirtualKeyboard', 'd3dcompiler',
           'qtuiotouchplugin', 'qgenericbearer', 'qnetworklistmanager', 'qtlsbackend', 'qsvg', 'qpdf', 'qtiff', 'qwebp', 'qgif', 'qicns', 'qwbmp', 'qtga')
a.binaries = [b for b in a.binaries if not any(u.lower() in b[0].lower() for u in _UNUSED)]
a.datas = [d for d in a.datas if 'translations' not in os.path.normpath(d[0]).split(os.sep)]
pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name='Caravan4URecomp-Launcher',
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=True,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=['assets/caravan4u.ico'],
    version='c4u_version_info.txt',
)
