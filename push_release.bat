:: Change drive & path
cd /D %~dp0

@echo off
REM Komut penceresinin Türkçe karakterleri doğru göstermesi için kod sayfasını ayarla
chcp 65001 > nul

REM --- KULLANICI AYARI: LÜTFEN BU YOLU KENDİ BİLGİSAYARINIZA GÖRE DÜZENLEYİN ---
REM
REM Bu, build numarasının tutuldugu .txt dosyasinin tam yoludur.
REM Private projenizin olduğu yerdeki version_number.txt dosyasını göstermeli.
set "BUILD_NUM_FILE_PATH=E:\_CODE_\AutoDoorV2\version_build.txt"
REM ---------------------------------------------------------------------------


cd release

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

REM 1. Adım: Klasördeki tüm yeni/değişmiş dosyaları Git'e ekle
git add .
echo - Tum degisiklikler eklendi (git add .).

REM 2. Adım: Otomatik build numarası ile commit yap
REM Commit mesajini dogrudan ve tek tirnak icinde belirtmek daha guvenli olabilir.
git commit -m "Build %BUILD_NUM%"
echo - Degisiklikler commit edildi. Mesaj: "Release Build %BUILD_NUM% surumu yayinlandi."

REM 3. Adım: Değişiklikleri GitHub'a gönder (push)
git push origin main
echo - Degisiklikler GitHub'a gonderildi (git push).
echo.
echo ===========================================
echo == Islem basariyla tamamlandi!           ==
echo ===========================================
echo.

REM Pencerenin hemen kapanmaması için bekle
pause