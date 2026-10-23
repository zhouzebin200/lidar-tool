#!/usr/bin/env python3
"""Write an absolute PCAP path into the EM4 replay configuration."""
import argparse
import json
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('pcap', type=Path)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
capture = args.pcap.expanduser().resolve()
if not capture.is_file():
    parser.error('capture file does not exist: ' + str(capture))
root = Path(__file__).resolve().parents[1]
template = root / 'src/rslidar_sdk/config/em4_pcap.yaml'
destination = (args.output or root / 'em4_replay.yaml').resolve()
lines = template.read_text().splitlines()
text = '\n'.join('      pcap_path: ' + json.dumps(str(capture))
                 if line.strip().startswith('pcap_path:') else line for line in lines) + '\n'
destination.write_text(text)
print(destination)
