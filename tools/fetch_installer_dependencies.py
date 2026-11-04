#!/usr/bin/env python3
"""Fetch pinned Shizuku API AARs from Maven Central; never disables TLS checks."""
import hashlib
from pathlib import Path
import ssl
import urllib.request
import zipfile

ROOT=Path(__file__).resolve().parents[1]
VERSION='13.1.5'
HASHES={
    'api':'4def9bde498ef8626614c2fc5db9af4749c86f16f6c33e3f5658d35e70bab59b',
    'provider':'b0f18cd9812464ec171c53cac93a819fe411718a3965c311f01eb4de265381b3',
    'shared':'4659642c9339be0a26e9c65bb8648f7ad6d8f4a465f557993ccbc78802381635',
    'aidl':'33fe7191cdd69fcb66d649264f3b0c47acb2f3d6343afc05b98dbbff6f221963',
}
def jars(download=False):
    folder=ROOT/'build/shizuku-deps'; folder.mkdir(parents=True,exist_ok=True)
    result=[]
    for name,sha in HASHES.items():
        aar=folder/(name+'.aar')
        if not aar.is_file():
            if not download: raise RuntimeError('Run python tools/fetch_installer_dependencies.py once before building')
            try:
                import certifi
                context=ssl.create_default_context(cafile=certifi.where())
            except ImportError: context=ssl.create_default_context()
            url=f'https://repo.maven.apache.org/maven2/dev/rikka/shizuku/{name}/{VERSION}/{name}-{VERSION}.aar'
            data=urllib.request.urlopen(url,context=context,timeout=45).read()
            if hashlib.sha256(data).hexdigest()!=sha: raise RuntimeError('Dependency SHA256 mismatch: '+name)
            aar.write_bytes(data)
        if hashlib.sha256(aar.read_bytes()).hexdigest()!=sha: raise RuntimeError('Cached dependency SHA256 mismatch: '+name)
        path=folder/(name+'.jar')
        with zipfile.ZipFile(aar) as z: path.write_bytes(z.read('classes.jar'))
        result.append(path)
    return result
if __name__=='__main__':
    for path in jars(download=True): print(path)
