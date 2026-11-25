"""构建可复现的嵌入式 gzip 资源。"""
import gzip
from pathlib import Path
import sys

source, destination = map(Path, sys.argv[1:])
destination.write_bytes(gzip.compress(source.read_bytes(), compresslevel=9, mtime=0))
