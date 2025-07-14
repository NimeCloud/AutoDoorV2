# scripts/version_script.py

import os
import json
import shutil
import hashlib  # <<< YENİ: Hash hesaplama için gerekli kütüphane
from SCons.Script import Import # type: ignore

# --- Yapılandırma ---
GITHUB_USER = "NimeCloud"
REPO_NAME = "GateControlFirmware"
BRANCH_NAME = "main"
RELEASE_DIR = "release"
# --------------------

Import("env")

# =================================================================
# Versiyonlama ve İsimlendirme (Bu kısımda değişiklik yok)
# =================================================================
print("--- Running Version & Naming Script ---")

version_major_file = 'version_major.txt'
version_minor_file = 'version_minor.txt'
version_build_file = 'version_build.txt'

try:
    with open(version_build_file, 'r') as f:
        build_number = int(f.read().strip()) + 1
except:
    build_number = 1
with open(version_build_file, 'w') as f:
    f.write(str(build_number))

try:
    with open(version_major_file, 'r') as f:
        major_version = int(f.read().strip())
except:
    major_version = 1

try:
    with open(version_minor_file, 'r') as f:
        minor_version = int(f.read().strip())
except:
    minor_version = 0

full_version_str = f"{major_version}.{minor_version}.{build_number}"
print(f"New Version: {full_version_str}")

version_h_content = f"""#pragma once
// Otomatik oluşturuldu - DO NOT EDIT
#define FW_VERSION_MAJOR {major_version}
#define FW_VERSION_MINOR {minor_version}
#define FW_VERSION_BUILD {build_number}
"""
with open('src/version.h', 'w') as f:
    f.write(version_h_content)
print("Generated src/version.h")

new_prog_name = f"gate_firmware_{build_number}"
env.Replace(PROGNAME=new_prog_name) # type: ignore
print(f"PROGNAME set to: {new_prog_name}")


# =================================================================
# Derleme Sonrası Kopyalama ve HASH HESAPLAMA (GÜNCELLENMİŞ)
# =================================================================
def post_build_actions(source, target, env):
    prog_name = env.get("PROGNAME")
    project_dir = env.get("PROJECT_DIR")
    
    # Hedef klasörü oluştur (eğer yoksa)
    release_path = os.path.join(project_dir, RELEASE_DIR)
    if not os.path.exists(release_path):
        os.makedirs(release_path)

    print(f"--- Running Post-Build Actions ---")
    
    # 1. Adım: Firmware (.bin) dosyasını kopyala
    source_firmware_path = str(target[0])
    dest_firmware_path = os.path.join(release_path, f"{prog_name}.bin")
    print(f"Copying '{source_firmware_path}' to '{dest_firmware_path}'")
    shutil.copy(source_firmware_path, dest_firmware_path)
    print("Firmware bin successfully copied to release folder.")

    # --- YENİ EKLENEN KISIM: HASH HESAPLAMA ---
    # 2. Adım: Kopyalanan .bin dosyasının SHA-256 hash'ini hesapla
    print(f"Calculating SHA-256 hash for '{dest_firmware_path}'...")
    sha256_hash = hashlib.sha256()
    with open(dest_firmware_path, "rb") as f:
        # Belleği verimli kullanmak için dosyayı parçalar halinde oku
        for byte_block in iter(lambda: f.read(4096), b""):
            sha256_hash.update(byte_block)
    firmware_hash_str = sha256_hash.hexdigest()
    print(f"Calculated Hash: {firmware_hash_str}")
    # --- BİTİŞ ---

    # --- YENİ EKLENEN KISIM: version.json GÜNCELLEME ---
    # 3. Adım: version.json dosyasını oluştur ve hash'i ekle
    new_firmware_url = f"https://raw.githubusercontent.com/{GITHUB_USER}/{REPO_NAME}/{BRANCH_NAME}/{RELEASE_DIR}/{prog_name}.bin"
    version_data = {
        "latest_version": full_version_str,
        "firmware_url": new_firmware_url,
        "firmware_hash": firmware_hash_str  # Hash değerini ekle
    }
    
    # Hem proje ana dizinine hem de release klasörüne kaydet
    for path in [project_dir, release_path]:
        dest_json_path = os.path.join(path, "version.json")
        with open(dest_json_path, 'w') as f:
            json.dump(version_data, f, indent=4)
        print(f"version.json successfully created/updated at '{dest_json_path}'.")
    # --- BİTİŞ ---

    print("--------------------------------------")


# =================================================================
# Kopyalama Eylemini PlatformIO'ya Tanımlama
# =================================================================
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", post_build_actions) # type: ignore