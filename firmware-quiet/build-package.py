from pathlib import Path
import hashlib
import json
import os
import subprocess
import sys

b = Path('build')
cfg = Path('sdkconfig').read_text()
required = ['CONFIG_BOOTLOADER_LOG_LEVEL_NONE=y', 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y',
            'CONFIG_ESP_CONSOLE_SECONDARY_NONE=y', 'CONFIG_ESP_SYSTEM_PANIC_SILENT_REBOOT=y',
            'CONFIG_ZB_ZCZR=y']
for setting in required:
    assert setting in cfg.splitlines(), 'Missing required setting: ' + setting
assert 'CONFIG_ESP_CONSOLE_UART_DEFAULT=y' not in cfg
assert 'CONFIG_ESP_CONSOLE_UART_CUSTOM=y' not in cfg
assert 'CONFIG_BOOT_ROM_LOG_ALWAYS_OFF=y' not in cfg, 'eFuse must only be changed with explicit user confirmation'
args = json.loads((b / 'flasher_args.json').read_text())
parts = []
for address, file in args['flash_files'].items():
    parts.extend([address, file])
subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32h2', 'merge-bin',
                '--output', 'MD-GATE-H2-USB.bin', '--pad-to-size', '4MB', *parts], cwd=b, check=True)
binary = b / 'MD-GATE-H2-USB.bin'
assert binary.stat().st_size == 4194304
manifest = {'version': '1.7.2-txrx01-USB', 'git_commit': os.environ.get('GITHUB_SHA'),
            'chip': 'esp32h2', 'flash_offset': '0x0', 'flash_size': 4194304,
            'sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'profile': 'USB_ONLY', 'required_uart_print_control': 3,
            'hardware_verified': False, 'bootloader_logs': 'NONE', 'idf_console': 'USB_SERIAL_JTAG',
            'pins': {'s1': 24, 's2': 23, 'closed': 0, 'open': 1}}
(b / 'build-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
