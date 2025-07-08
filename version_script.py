import os
Import("env")

def increment_build():
    # Build numarası dosya yolu
    build_file = 'build_number.txt'
    
    # Build numarasını oku veya 1'den başlat
    try:
        with open(build_file, 'r') as f:
            build_num = int(f.read().strip()) + 1
    except:
        build_num = 1
    
    # Yeni build numarasını kaydet
    with open(build_file, 'w') as f:
        f.write(str(build_num))
    
    # version.h dosyasını oluştur/güncelle
    version_content = f"""#pragma once
// Otomatik oluşturuldu
#define FW_VERSION_MAJOR 1
#define FW_VERSION_MINOR 0
#define FW_VERSION_BUILD {build_num}
"""
    
    with open('src/version.h', 'w') as f:
        f.write(version_content)
    
    print(f"Build numarası güncellendi: {build_num}")

# PlatformIO build öncesi bu fonksiyonu çalıştır
increment_build()