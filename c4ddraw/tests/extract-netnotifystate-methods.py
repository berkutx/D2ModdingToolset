"""Extract the current MSS notification bodies; no IDA, game or installed SDK needed."""
import argparse
import hashlib
import json
from pathlib import Path

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, default=root / '.diagnostics/netnotifystate-tests/generated')
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
sources = {}


def extract(relative, signatures, output):
    raw = (root / relative).read_bytes()
    text = raw.decode('utf-8').replace('\r\n', '\n')
    bodies = []
    for signature in signatures:
        if text.count(signature) != 1:
            raise RuntimeError('Expected one method signature: ' + signature)
        start = text.index(signature)
        end = text.index('\n}\n', start) + 3
        bodies.append(text[start:end])
    extracted = '\n'.join(bodies)
    (args.output / output).write_text(extracted, encoding='utf-8')
    sources[relative] = {
        'sha256': hashlib.sha256(raw).hexdigest(),
        'methods': signatures,
        'extracted_sha256': hashlib.sha256(extracted.encode()).hexdigest(),
    }


extract('mss32/src/netcustompeer.cpp', [
    'bool CNetCustomPeer::IsPacketNotificationSent() const\n',
    'void CNetCustomPeer::ResetPacketNotification()\n',
    'void CNetCustomPeer::UpdateThreadCallback(',
    'void CNetCustomPeer::SendPacketNotification()\n',
], 'netnotifystate-peer.generated.inc')
extract('mss32/src/netcustomservice.cpp', [
    'CNetCustomService* CNetCustomService::get()\n',
    'void __fastcall CNetCustomService::peerProcessEventCallback(',
], 'netnotifystate-service.generated.inc')
metadata = {
    'sources': sources,
    'production_state_header': 'c4ddraw/features/netnotifystate.h',
    'production_state_sha256': hashlib.sha256((root / 'c4ddraw/features/netnotifystate.h').read_bytes()).hexdigest(),
    'substitutes': ['RakPeer FIFO and packet callbacks', 'UIManager accessor',
                    'Midgard current-service storage', 'native safe-boundary classification'],
    'real': ['worker and UI OS threads', 'PostMessage/PeekMessage/DispatchMessage',
             'std::atomic MSS flag', 'production WakeState header',
             'six extracted MSS notification and receiver methods'],
    'controlled_schedule': 'Event barrier after real PostMessage returns, before extracted sender stores its atomic flag',
}
(args.output / 'extraction.json').write_text(json.dumps(metadata, indent=2) + '\n', encoding='utf-8')
print('Extracted six current MSS methods into ' + str(args.output))
