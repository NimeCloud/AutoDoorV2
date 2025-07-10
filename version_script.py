import os
import json
import shutil

Import("env")

# --- KULLANICI AYARI ---
RELEASES_DIR = "E:/_CODE_/AutoDoorV2/release" # Lütfen bu yolu kendi bilgisayarınıza göre güncelleyin
# -------------------------

# --- Proje Bilgileri ---
GITHUB_USER = "NimeCloud"
REPO_NAME = "GateControlFirmware" # Public deponun adı
BRANCH_NAME = "main"

def copy_artifacts_to_releases(source, target, env):
    print("\n--- Kopyalama İşlemi Başladı ---")
    
    releases_dir_path = env.subst(RELEASES_DIR)
    print(f"[TEŞHİS] Releases klasor yolu: {releases_dir_path}")
    if not os.path.isdir(releases_dir_path):
        print(f"HATA: Belirtilen releases klasörü bulunamadı: {releases_dir_path}")
        return

    # --- EN GÜVENİLİR YÖNTEMLE YOL OLUŞTURMA ---
    # 1. Projenin mutlak yolunu al (örn: E:\_CODE_\AutoDoorV2)
    project_dir = env.subst("$PROJECT_DIR")
    # 2. Derleme ortamının adını al (örn: 'gate')
    env_name = env['PIOENV']
    # 3. .bin dosyasının adını al (örn: gate_firmware_82.bin)
    firmware_filename = env.get("PROGNAME") + ".bin"
    
    # 4. Bu parçaları birleştirerek .bin dosyasının tam ve mutlak yolunu oluştur
    # Sonuç: E:\_CODE_\AutoDoorV2\.pio\build\gate\gate_firmware_82.bin
    firmware_source_path = os.path.join(project_dir, ".pio", "build", env_name, firmware_filename)
    # --- DEĞİŞİKLİĞİN SONU ---
    
    print(f"[TEŞHİS] Yeni Yöntemle Kaynak .bin Yolu: {firmware_source_path}")
    
    if os.path.exists(firmware_source_path):
        print("[TEŞHİS] Kaynak .bin dosyasi bulundu mu? Evet.")
    else:
        print("[TEŞHİS] Kaynak .bin dosyasi bulundu mu? HAYIR! Bu dosya bulunamadi.")
        print("--- Kopyalama İşlemi Başarısız ---")
        return

    firmware_dest_path = os.path.join(releases_dir_path, firmware_filename)
    
    try:
        shutil.copy(firmware_source_path, firmware_dest_path)
        print(f"BAŞARILI: .bin dosyasi kopyalandi.")
    except Exception as e:
        print(f"HATA: .bin dosyasi kopyalanirken bir hata olustu: {e}")

    # version.json dosyasını kopyala
    version_json_source_path = os.path.join(project_dir, "version.json")
    
    try:
        shutil.copy(version_json_source_path, releases_dir_path)
        print("BAŞARILI: version.json dosyasi kopyalandi.")
    except Exception as e:
        print(f"HATA: version.json dosyasi kopyalanirken bir hata olustu: {e}")

    print("--- Kopyalama İşlemi Tamamlandı ---\n")

def create_version_files():
    build_file = 'build_number.txt'
    try:
        with open(build_file, 'r') as f:
            build_number = int(f.read().strip()) + 1
    except:
        build_number = 1
    
    with open(build_file, 'w') as f:
        f.write(str(build_number))
    
    print(f"Build numarası güncellendi: {build_number}")

    major_version = 1
    minor_version = 0    
    version_h_content = f"""#pragma once
// Otomatik oluşturuldu
#define FW_VERSION_MAJOR {major_version}
#define FW_VERSION_MINOR {minor_version}
#define FW_VERSION_BUILD {build_number}
"""
    with open('src/version.h', 'w') as f:
        f.write(version_h_content)
    
    new_prog_name = f"gate_firmware_{build_number}"
    env.Replace(PROGNAME=new_prog_name)

    latest_version_str = f"{major_version}.{minor_version}.{build_number}"
    new_bin_url = f"https://raw.githubusercontent.com/{GITHUB_USER}/{REPO_NAME}/{BRANCH_NAME}/{new_prog_name}.bin"
    
    version_data = {
        "latest_version": latest_version_str,
        "bin_url": new_bin_url
    }

    with open('version.json', 'w') as f:
        json.dump(version_data, f, indent=2)
        
    print("version.json başarıyla güncellendi.")

# --- Script'in Ana Akışı ---
create_version_files()
env.AddPostAction(os.path.join("$BUILD_DIR", "${PROGNAME}.bin"), copy_artifacts_to_releases)
