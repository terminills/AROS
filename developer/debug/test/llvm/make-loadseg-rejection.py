"""Copy an ELF64 little-endian image and make one relocation unsupported.

Use only as a LoadSeg rejection fixture; never execute the resulting image.
The source is unchanged and an existing destination is never overwritten.
"""
import argparse
import json
from pathlib import Path
import shutil
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path)
parser.add_argument("destination", type=Path)
args = parser.parse_args()

with args.source.open("rb") as source:
    header = source.read(64)
    if len(header) != 64 or header[:6] != b"\x7fELF\x02\x01":
        raise SystemExit("expected an ELF64 little-endian image")
    section_offset = struct.unpack_from("<Q", header, 40)[0]
    section_size, section_count = struct.unpack_from("<HH", header, 58)
    if section_size != 64:
        raise SystemExit("unexpected ELF section-header size")
    source.seek(section_offset)
    first = source.read(64)
    if not section_count:
        section_count = struct.unpack_from("<Q", first, 32)[0]
    source.seek(section_offset)
    headers = source.read(section_count * 64)
    if len(headers) != section_count * 64:
        raise SystemExit("truncated section headers")
    sections = [struct.unpack_from("<IIQQQQIIQQ", headers, i * 64)
                for i in range(section_count)]
    # SHT_RELA referring to a SHF_ALLOC section: the loader will process it.
    index = next((i for i, section in enumerate(sections)
                  if section[1] == 4 and section[5] >= 24
                  and section[7] < section_count
                  and sections[section[7]][2] & 2), None)
    if index is None:
        raise SystemExit("no applicable RELA section")
    offset = sections[index][4] + 8
    source.seek(offset)
    old_info = struct.unpack("<Q", source.read(8))[0]
    new_info = (old_info & 0xffffffff00000000) | 0x7ffffffe
    source.seek(0)
    with args.destination.open("xb") as destination:
        shutil.copyfileobj(source, destination)

with args.destination.open("r+b") as destination:
    destination.seek(offset)
    destination.write(struct.pack("<Q", new_info))

print(json.dumps({"source": str(args.source),
                  "fixture": str(args.destination),
                  "relocation_section": index, "file_offset": offset,
                  "old_info": old_info, "new_info": new_info}, indent=2))
