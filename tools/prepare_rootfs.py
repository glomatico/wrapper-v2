#!/usr/bin/env python3
"""Stage a new rootfs from the user's APK/APKM and pinned Android system files."""
import argparse, hashlib, io, json, os, shutil, tempfile, zipfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
PINS=json.loads((ROOT/"profiles/apple-music-1109-x86_64.json").read_text())
def digest(data):return hashlib.sha256(data).hexdigest()
def extract_apple(source):
    with zipfile.ZipFile(source) as outer:
        if "split_config.x86_64.apk" in outer.namelist():
            if "base.apk" not in outer.namelist() or outer.getinfo("base.apk").file_size>128*1024*1024:raise ValueError("missing or oversized base APK")
            with zipfile.ZipFile(io.BytesIO(outer.read("base.apk"))) as base:
                if digest(base.read("AndroidManifest.xml"))!=PINS["base_manifest_sha256"]:raise ValueError("unsupported APK build")
            info=outer.getinfo("split_config.x86_64.apk")
            if info.file_size>128*1024*1024:raise ValueError("oversized APK split")
            archive=zipfile.ZipFile(io.BytesIO(outer.read(info)))
        else:archive=outer
        try:
            if "AndroidManifest.xml" not in archive.namelist() or digest(archive.read("AndroidManifest.xml"))!=PINS["split_manifest_sha256"]:raise ValueError("unsupported x86_64 APK build")
            files={}
            for name,expected in PINS["apple_libraries"].items():
                path="lib/x86_64/"+name
                matches=[i for i in archive.infolist() if i.filename==path]
                if len(matches)!=1 or matches[0].file_size>128*1024*1024:raise ValueError("missing or ambiguous pinned library: "+name)
                data=archive.read(matches[0])
                if digest(data)!=expected:raise ValueError("unsupported Apple library: "+name)
                if data[:6]!=b"\x7fELF\x02\x01" or int.from_bytes(data[18:20],"little")!=62:raise ValueError("wrong ELF architecture")
                files[name]=data
            return files
        finally:
            if archive is not outer:archive.close()
def stage(apk,system,worker,output,ca):
    output=output.absolute()
    if output.exists():raise ValueError("output must not exist")
    # Verify every source before creating the rootfs, and only copy allowlisted paths.
    apple=extract_apple(apk)
    android={}
    for rel,expected in PINS["android_system"].items():
        source=system/rel
        if source.is_symlink() or not source.is_file():raise ValueError("missing Android artifact: "+rel)
        data=source.read_bytes()
        if digest(data)!=expected:raise ValueError("unsupported Android artifact: "+rel)
        android[rel]=data
    executable=worker.read_bytes()
    if executable[:6]!=b"\x7fELF\x02\x01" or int.from_bytes(executable[18:20],"little")!=62:raise ValueError("worker must be x86_64 ELF")
    certificate=ca.read_bytes()
    if b"-----BEGIN CERTIFICATE-----" not in certificate:raise ValueError("invalid CA bundle")
    output.parent.mkdir(parents=True,exist_ok=True)
    temp=Path(tempfile.mkdtemp(prefix=".worker-rootfs-",dir=output.parent))
    try:
        for rel,data in android.items():
            path=temp/"system"/rel;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);path.chmod(0o755 if rel.startswith("bin/") else 0o644)
        for name,data in apple.items():
            path=temp/"system/lib64"/name;path.write_bytes(data);path.chmod(0o644)
        path=temp/"system/bin/apple-music-worker";path.write_bytes(executable);path.chmod(0o755)
        for rel in ["proc","dev","tmp","data/data/com.apple.android.music/files/mpl_db","etc/ssl/certs"]:(temp/rel).mkdir(parents=True,exist_ok=True)
        (temp/"tmp").chmod(0o1777)
        (temp/"data/data/com.apple.android.music/files").chmod(0o700)
        (temp/"etc/ssl/certs/ca-certificates.crt").write_bytes(certificate)
        # No overwrite: mkdir reserves the destination before moving our verified files.
        output.mkdir()
        for child in temp.iterdir():shutil.move(str(child),output/child.name)
    finally:shutil.rmtree(temp)
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--apk",type=Path,required=True)
    p.add_argument("--android-system",type=Path,help="directory containing bin/, lib64/, usr/")
    p.add_argument("--extract-libraries",type=Path,help="only extract verified Apple libraries into a new directory")
    p.add_argument("--worker",type=Path,default=ROOT/"build/android/apple-music-worker")
    p.add_argument("--output",type=Path,default=ROOT/"build/rootfs")
    p.add_argument("--ca-bundle",type=Path,default=Path("/etc/ssl/certs/ca-certificates.crt"))
    a=p.parse_args()
    try:
        if a.extract_libraries:
            files=extract_apple(a.apk)
            a.extract_libraries.mkdir(parents=True,exist_ok=False)
            for name,data in files.items():(a.extract_libraries/name).write_bytes(data)
            print("Extracted verified Apple Music build 1109 x86_64 libraries.")
            return
        if not a.android_system:p.error("--android-system is required for rootfs staging")
        stage(a.apk,a.android_system,a.worker,a.output,a.ca_bundle)
    except (ValueError,OSError,zipfile.BadZipFile) as e:p.exit(1,str(e)+"\n")
    print("Staged verified x86_64 rootfs; no account data included.")
if __name__=="__main__":main()
