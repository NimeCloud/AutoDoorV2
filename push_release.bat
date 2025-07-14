:: Change drive & path
cd /D %~dp0

@echo off
REM Komut penceresinin Türkçe karakterleri doğru göstermesi için kod sayfasını ayarla
chcp 65001 > nul

REM --- KULLANICI AYARI: LÜTFEN BU YOLU KENDİ BİLGİSAYARINIZA GÖRE DÜZENLEYİN ---
set "BUILD_NUM_FILE_PATH=E:\_CODE_\AutoDoorV2\version_build.txt"
REM ---------------------------------------------------------------------------

REM Projenin ana dizinine git (release klasörüne değil)
REM .git klasörünün olduğu yere gitmeliyiz.
REM cd release satırı kaldırıldı. Git komutları ana dizinden çalıştırılmalı.

echo.
echo ===========================================
echo == GateControl Firmware Release Gonderici ==
echo ===========================================
echo.

REM Build numarası dosyasının var olup olmadığını kontrol et
if not exist "%BUILD_NUM_FILE_PATH%" (
    echo HATA: Build numarasi dosyasi bulunamadi!
    echo Lutfen script icindeki yolu kontrol edin:
    echo %BUILD_NUM_FILE_PATH%
    pause
    exit /b
)

REM Dosyanın ilk satırını okuyup BUILD_NUM değişkenine ata
set /p BUILD_NUM=<"%BUILD_NUM_FILE_PATH%"

echo Bulunan en guncel build numarasi: %BUILD_NUM%
echo.
echo Git islemleri baslatiliyor...
echo.

REM 1. Adım: Tüm yeni/değişmiş dosyaları Git'e ekle
git add .
echo - Tum degisiklikler eklendi (git add .).

REM 2. Adım: Otomatik build numarası ile commit yap
git commit -m "Build %BUILD_NUM%"
if errorlevel 1 (
    echo HATA: Commit basarisiz oldu. Muhtemelen commit edilecek bir degisiklik yok.
    pause
    exit /b
)
echo - Degisiklikler commit edildi. Mesaj: "Build %BUILD_NUM%"

REM --- YENİ EKLENEN ADIM ---
REM 3. Adım: Değişiklikleri göndermeden önce sunucudaki son durumu çek (pull)
echo - Sunucudaki degisiklikler cekiliyor (git pull)...
git pull origin main
if errorlevel 1 (
    echo HATA: 'git pull' basarisiz oldu. Muhtemelen bir 'merge conflict' (birleştirme çakışması) var.
    echo Lutfen durumu manuel olarak VS Code veya Git Bash uzerinden cozun.
    pause
    exit /b
)
echo - Depo basariyla guncellendi.

REM 4. Adım: Değişiklikleri GitHub'a gönder (push)
echo - Degisiklikler GitHub'a gonderiliyor (git push)...
git push origin main
if errorlevel 1 (
    echo HATA: 'git push' basarisiz oldu.
    pause
    exit /b
)
echo - Degisiklikler GitHub'a gonderildi.
echo.
echo ===========================================
echo == Islem basariyla tamamlandi!           ==
echo ===========================================
echo.

REM Pencerenin hemen kapanmaması için bekle
pause