# scripts/version_script.py

import os
import json
import shutil
from SCons.Script import Import

# --- Yapılandırma ---
GITHUB_USER = "NimeCloud"
REPO_NAME = "GateControlFirmware"
BRANCH_NAME = "main"
RELEASE_DIR = "release" 
# --------------------

Import("env")

# =================================================================
# Versiyonlama ve İsimlendirme (Doğrudan Çalıştırılan Kısım)
# =================================================================
print("--- Running Version & Naming Script ---")

# ... (Bu bölümün tamamı önceki mesajdakiyle aynı, değişiklik yok) ...
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
env.Replace(PROGNAME=new_prog_name)
print(f"PROGNAME set to: {new_prog_name}")

new_bin_url = f"https://raw.githubusercontent.com/{GITHUB_USER}/{REPO_NAME}/{BRANCH_NAME}/{new_prog_name}.bin"
version_data = {
    "latest_version": full_version_str,
    "bin_url": new_bin_url
}
with open('version.json', 'w') as f:
    json.dump(version_data, f, indent=2)
print("version.json successfully updated.")

# =================================================================
# Derleme Sonrası Kopyalama Fonksiyonu (GÜNCELLENMİŞ)
# =================================================================
def post_build_copy(source, target, env):
    prog_name = env.get("PROGNAME")
    project_dir = env.get("PROJECT_DIR")
    
    # Hedef klasörü oluştur (eğer yoksa)
    release_path = os.path.join(project_dir, RELEASE_DIR)
    if not os.path.exists(release_path):
        os.makedirs(release_path)

    print(f"--- Running Post-Build Copy Script ---")
    
    # 1. Adım: Firmware (.bin) dosyasını kopyala
    source_bin_path = str(target[0])
    dest_bin_path = os.path.join(release_path, f"{prog_name}.bin")
    print(f"Copying '{source_bin_path}' to '{dest_bin_path}'")
    shutil.copy(source_bin_path, dest_bin_path)
    print("Firmware bin successfully copied to release folder.")

    # --- YENİ EKLENEN KISIM ---
    # 2. Adım: version.json dosyasını kopyala
    source_json_path = os.path.join(project_dir, "version.json")
    dest_json_path = os.path.join(release_path, "version.json")
    print(f"Copying '{source_json_path}' to '{dest_json_path}'")
    shutil.copy(source_json_path, dest_json_path)
    print("version.json successfully copied to release folder.")
    # --- BİTİŞ ---

    print("--------------------------------------")


# =================================================================
# Kopyalama Eylemini PlatformIO'ya Tanımlama
# =================================================================
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", post_build_copy)